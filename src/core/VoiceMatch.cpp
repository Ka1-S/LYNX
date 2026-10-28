#include "core/VoiceMatch.h"
#include "core/VoxStrip.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>

namespace lynx::vmatch
{
namespace
{
constexpr double pi = 3.141592653589793;
constexpr double refRate = 48000.0; // the rate the calibration was measured at

inline double dbToGain (double db) noexcept { return std::pow (10.0, db / 20.0); }
inline double gainToDb (double g) noexcept { return 20.0 * std::log10 (std::max (g, 1e-12)); }
inline double coefMs (double ms, double sr) noexcept { return ms <= 0.0 ? 1.0 : 1.0 - std::exp (-1000.0 / (ms * sr)); }
inline double roundTo (double v, double step) noexcept { return std::round (v / step) * step; }

// piecewise-linear lookup in a sorted table
double interp (const float* xs, const float* ys, std::size_t N, double x) noexcept
{
    if (x <= xs[0])
        return ys[0];
    if (x >= xs[N - 1])
        return ys[N - 1];
    const auto* it = std::upper_bound (xs, xs + N, (float) x);
    const auto i = (std::size_t) (it - xs) - 1;
    const auto t = (x - xs[i]) / (xs[i + 1] - xs[i]);
    return ys[i] + t * (ys[i + 1] - ys[i]);
}

// bilinear transform of an analog biquad (n2 s^2 + n1 s + n0) / (s^2 + d1 s + d0) with s = K (1 - z^-1) / (1 + z^-1)
void bilinear (const double n[3], const double d[2], double K, Biquad& b) noexcept
{
    const double K2 = K * K;
    const double b0 = n[0] * K2 + n[1] * K + n[2], b1 = 2.0 * (n[2] - n[0] * K2), b2 = n[0] * K2 - n[1] * K + n[2];
    const double a0 = K2 + d[0] * K + d[1], a1 = 2.0 * (d[1] - K2), a2 = K2 - d[0] * K + d[1];
    b.set (b0, b1, b2, a0, a1, a2);
}

double biquadMagnitude (const Biquad& b, double w) noexcept
{
    const double c1 = std::cos (w), s1 = std::sin (w), c2 = std::cos (2 * w), s2 = std::sin (2 * w);
    const double nr = b.b0 + b.b1 * c1 + b.b2 * c2, ni = -(b.b1 * s1 + b.b2 * s2);
    const double dr = 1.0 + b.a1 * c1 + b.a2 * c2, di = -(b.a1 * s1 + b.a2 * s2);
    return std::sqrt ((nr * nr + ni * ni) / std::max (dr * dr + di * di, 1e-300));
}
} // namespace

namespace echannel
{
namespace
{
#include "core/VoiceMatchTables.inc"

constexpr double gainNodes[21] = { -15, -13.5, -12, -10.5, -9, -7.5, -6, -4.5, -3, -1.5, 0, 1.5, 3, 4.5, 6, 7.5, 9, 10.5, 12, 13.5, 15 };

// Catmull-Rom over the gain nodes of one segment ([-15,-7.5], [-7.5,0], [0,7.5], [7.5,15]: the band's law changes
// slope at the segment ends, measured), `at(i)` = the feature vector of gain node i
template <typename At>
void gainInterp (double g, At at, double out[5]) noexcept
{
    g = std::clamp (g, -15.0, 15.0);
    const int seg = g < -7.5 ? 0 : (g < 0.0 ? 1 : (g < 7.5 ? 2 : 3));
    const int lo = seg * 5, hi = lo + 5; // node indexes of the segment ends
    int i = std::clamp ((int) std::floor ((g + 15.0) / 1.5), lo, hi - 1);
    const double t = (g - gainNodes[i]) / 1.5;
    double p0[5], p1[5], p2[5], p3[5];
    at (i, p1);
    at (i + 1, p2);
    if (i > lo) at (i - 1, p0); else for (int k = 0; k < 5; ++k) p0[k] = 2 * p1[k] - p2[k];
    if (i + 2 <= hi) at (i + 2, p3); else for (int k = 0; k < 5; ++k) p3[k] = 2 * p2[k] - p1[k];
    const double t2 = t * t, t3 = t2 * t;
    for (int k = 0; k < 5; ++k)
    {
        const double m1 = 0.5 * (p2[k] - p0[k]), m2 = 0.5 * (p3[k] - p1[k]);
        out[k] = (2 * t3 - 3 * t2 + 1) * p1[k] + (t3 - 2 * t2 + t) * m1 + (-2 * t3 + 3 * t2) * p2[k] + (t3 - t2) * m2;
    }
}

struct Axis { int i; double t; };
Axis axis (const float* nodes, std::size_t N, double x) noexcept
{
    int i = 0;
    while (i < (int) N - 2 && x > nodes[i + 1])
        ++i;
    const double t = std::clamp ((x - nodes[i]) / (nodes[i + 1] - nodes[i]), 0.0, 1.0);
    return { i, t };
}

// the analog-domain biquad (normalised features x reference frequency) of one band, and its reference frequency
bool bandAnalog (int band, const Settings& s, double n[3], double d[2], double& wref) noexcept
{
    double g = 0.0, f = 0.0, q = 0.0;
    bool bell = false;
    switch (band)
    {
        case 0: g = s.lfGain; f = roundTo (std::clamp (s.lfHz, 30.0, 450.0), 1.0); bell = s.lfBell; break;
        case 1: g = s.lmfGain; f = roundTo (std::clamp (s.lmfKHz, 0.2, 2.5), 0.01) * 1000.0; q = s.lmfQ; break;
        case 2: g = s.hmfGain; f = roundTo (std::clamp (s.hmfKHz, 0.6, 7.0), 0.01) * 1000.0; q = s.hmfQ; break;
        default: g = s.hfGain; f = roundTo (std::clamp (s.hfKHz, 1.5, 16.0), 0.01) * 1000.0; bell = s.hfBell; break;
    }
    g = roundTo (g, 0.1);
    if (std::abs (g) < 0.05)
        return false;
    double u[5];
    if (band == 0 || band == 3)
    {
        const bool lo = band == 0;
        const float* feat = lo ? lfFeat : hfFeat;
        const double lw = lo ? interp (lfLawText, lfLawLogW, std::size (lfLawText), f) : interp (hfLawText, hfLawLogW, std::size (hfLawText), f);
        wref = std::exp (lw);
        const auto ax = lo ? axis (lfNodeLogW, std::size (lfNodeLogW), lw) : axis (hfNodeLogW, std::size (hfNodeLogW), lw);
        const int nf = (int) (lo ? std::size (lfNodeLogW) : std::size (hfNodeLogW)), type = bell ? 1 : 0;
        gainInterp (g, [&] (int gi, double* o) {
            const float* a = feat + ((type * 21 + gi) * nf + ax.i) * 5;
            for (int k = 0; k < 5; ++k)
                o[k] = (1 - ax.t) * a[k] + ax.t * a[5 + k];
        }, u);
    }
    else
    {
        const bool lo = band == 1;
        const float* feat = lo ? lmfFeat : hmfFeat;
        const double lw = lo ? interp (lmfLawText, lmfLawLogW, std::size (lmfLawText), f) : interp (hmfLawText, hmfLawLogW, std::size (hmfLawText), f);
        wref = std::exp (lw);
        const auto ax = lo ? axis (lmfNodeLogW, std::size (lmfNodeLogW), lw) : axis (hmfNodeLogW, std::size (hmfNodeLogW), lw);
        const auto aq = lo ? axis (lmfQv, std::size (lmfQv), dialFromQ (roundTo (q, 0.01))) : axis (hmfQv, std::size (hmfQv), dialFromQ (roundTo (q, 0.01)));
        const int nf = (int) (lo ? std::size (lmfNodeLogW) : std::size (hmfNodeLogW)), nq = (int) std::size (lmfQv);
        gainInterp (g, [&] (int gi, double* o) {
            for (int k = 0; k < 5; ++k)
                o[k] = 0.0;
            for (int df = 0; df < 2; ++df)
                for (int dq = 0; dq < 2; ++dq)
                {
                    const double w = (df ? ax.t : 1 - ax.t) * (dq ? aq.t : 1 - aq.t);
                    const float* a = feat + ((gi * nf + ax.i + df) * nq + aq.i + dq) * 5;
                    for (int k = 0; k < 5; ++k)
                        o[k] += w * a[k];
                }
        }, u);
    }
    n[0] = u[0];
    n[1] = u[1] * wref;
    n[2] = u[2] * wref * wref;
    d[0] = u[3] * wref;
    d[1] = u[4] * wref * wref;
    return true;
}

// At other sample rates the 48 kHz analog-domain response is kept at the band's own frequency: the prototype is
// scaled so that bilinear(K = 2 sr) maps that frequency where bilinear(K = 96000) mapped it at 48 kHz [model]
double rateScale (double wref, double sr) noexcept
{
    if (std::abs (sr - refRate) < 1.0)
        return 1.0;
    const double f0 = refRate / pi * std::atan (wref / (2.0 * refRate)); // the band's frequency in Hz (48 kHz design)
    const double fs = std::min (f0, 0.45 * sr);
    return (2.0 * sr * std::tan (pi * fs / sr)) / (2.0 * refRate * std::tan (pi * fs / refRate)) * (fs / f0);
}

// ---- round 148: the "4K" console's EQ and filters ([EXEC] SSL 4K E, official Complete Access trial, impulse responses
// at 48 kHz with the VCA and auto make-up off, three colours x every band at 8 gains, 4–5 frequencies and 5 Qs;
// tmp/vox148/k4: k4camp.py, k4eq.py, k4fit2.py, k4tables.py; REA_rodada148 §2). Each band is one second-order
// section whose (frequency, gain, Q) follow the dials by the fitted tables: the shelves are bilinear sections at 48 kHz
// (re-warped to keep their frequency at other rates), the bells follow the analog prototype's magnitude up to Nyquist
// (magnitude-matched), the filters are 12 dB/oct.
namespace k4
{
#include "core/Console4KTables.inc"

double lerp (double a, double b, double t) noexcept { return a + (b - a) * t; }

// position of x among sorted nodes (log axis when `logAxis`): index of the lower node and the fraction, clamped
Axis nodeAt (const float* nodes, int n, double x, bool logAxis) noexcept
{
    int i = 0;
    while (i < n - 2 && x > nodes[i + 1])
        ++i;
    const double a = logAxis ? std::log ((double) nodes[i]) : (double) nodes[i], b = logAxis ? std::log ((double) nodes[i + 1]) : (double) nodes[i + 1];
    const double v = logAxis ? std::log (std::max (x, 1e-9)) : x;
    return { i, std::clamp ((v - a) / (b - a), 0.0, 1.0) };
}

// one section from a row of 8 gain nodes { ratio, gain dB, Q }: piecewise linear between the nodes; between −2 and +2
// dB the ±2 node's frequency and Q with the gain scaled toward 0
void fromGainRow (const float (*row)[3], double g, double out[3]) noexcept
{
    g = std::clamp (g, -15.0, 15.0);
    if (std::abs (g) < 2.0)
    {
        const auto* nd = row[g < 0.0 ? 3 : 4];
        out[0] = nd[0];
        out[1] = nd[1] * std::abs (g) / 2.0;
        out[2] = nd[2];
        return;
    }
    const auto ax = nodeAt (gainNode, 8, g, false);
    for (int k = 0; k < 3; ++k)
        out[k] = lerp (row[ax.i][k], row[ax.i + 1][k], ax.t);
}

// a bilinear section from the analog (n2 s² + n1 s + n0) / (d2 s² + d1 s + d0), s normalised to w0 (rad/s), at the
// 48 kHz calibration; at other rates w0 is re-warped so the section's frequency stays where it was at 48 kHz
void bilinearAt (const double n[3], const double d[3], double f0, double sr, Biquad& b) noexcept
{
    double fd = refRate / pi * std::atan (pi * f0 / refRate); // the digital frequency f0 lands on at 48 kHz
    fd = std::min (fd, 0.495 * sr);
    const double w0 = 2.0 * sr * std::tan (pi * fd / sr);
    const double c = 2.0 * sr / w0, c2 = c * c;
    b.set (n[0] * c2 + n[1] * c + n[2], 2.0 * (n[2] - n[0] * c2), n[0] * c2 - n[1] * c + n[2],
           d[0] * c2 + d[1] * c + d[2], 2.0 * (d[2] - d[0] * c2), d[0] * c2 - d[1] * c + d[2]);
}

void shelf (bool low, double f0, double db, double q, double sr, Biquad& b) noexcept
{
    const double A = std::pow (10.0, db / 40.0), sa = std::sqrt (A) / q;
    if (low)
    {
        const double n[3] { A, A * sa, A * A }, d[3] { A, sa, 1.0 };
        bilinearAt (n, d, f0, sr, b);
    }
    else
    {
        const double n[3] { A * A, A * sa, A }, d[3] { 1.0, sa, A };
        bilinearAt (n, d, f0, sr, b);
    }
}

constexpr float qNode[5] = { 0.5f, 1.0f, 1.5f, 2.0f, 3.0f };

bool band (int which, const Settings& s, double sr, Biquad& out) noexcept
{
    const int c = std::clamp (s.colour, 0, 2);
    double g = 0.0, f = 0.0, q = 1.5;
    bool bell = false;
    switch (which)
    {
        case 0: g = s.lfGain; f = std::clamp (s.lfHz, 30.0, 450.0); bell = s.lfBell; break;
        case 1: g = s.lmfGain; f = std::clamp (s.lmfKHz, 0.2, 2.5) * 1000.0; q = s.lmfQ; break;
        case 2: g = s.hmfGain; f = std::clamp (s.hmfKHz, 0.6, 7.0) * 1000.0; q = s.hmfQ; break;
        default: g = s.hfGain; f = std::clamp (s.hfKHz, 1.5, 16.0) * 1000.0; bell = s.hfBell; break;
    }
    g = std::clamp (g, -15.0, 15.0);
    if (std::abs (g) < 0.05)
        return false;
    double v[3];
    if (which == 1 || which == 2)
    {
        const bool lo = which == 1;
        const auto& tab = lo ? lmfBell[c] : hmfBell[c];
        const auto aq = nodeAt (qNode, 5, std::clamp (q, 0.5, 3.0), true);
        double a[3], b[3];
        fromGainRow (tab[aq.i], g, a);
        fromGainRow (tab[aq.i + 1], g, b);
        for (int k = 0; k < 3; ++k)
            v[k] = lerp (a[k], b[k], aq.t);
        const auto* fn = lo ? lmfFreqNode : hmfFreqNode;
        const auto* fr = lo ? lmfFreqRatio[c] : hmfFreqRatio[c];
        const auto af = nodeAt (fn, 4, f, true);
        v[0] *= std::exp (lerp (std::log ((double) fr[af.i]), std::log ((double) fr[af.i + 1]), af.t));
        voxstrip::matchedPeak (out, sr, f * v[0], v[2], v[1]);
        return true;
    }
    const bool lo = which == 0;
    if (bell)
    {
        // the Bell type, measured at one point (100 Hz / 8 kHz, ±10 dB): its frequency ratio and Q, the gain law of the
        // mid bells (Q 1.5) scaled to its own ±10 dB gain [model]
        const auto* bt = lo ? lfbell[c] : hfbell[c];
        double m[3], m10[3];
        fromGainRow (lmfBell[c][2], g, m);
        fromGainRow (lmfBell[c][2], 10.0, m10);
        voxstrip::matchedPeak (out, sr, f * bt[0], bt[2], m[1] * bt[1] / m10[1]);
        return true;
    }
    const auto* fn = lo ? lfFreqNode : hfFreqNode;
    const int nf = lo ? 5 : 4;
    const auto af = nodeAt (fn, nf, f, true);
    double a[3], b[3];
    fromGainRow (lo ? lfShelf[c][af.i] : hfShelf[c][af.i], g, a);
    fromGainRow (lo ? lfShelf[c][af.i + 1] : hfShelf[c][af.i + 1], g, b);
    v[0] = std::exp (lerp (std::log (a[0]), std::log (b[0]), af.t));
    v[1] = lerp (a[1], b[1], af.t);
    v[2] = lerp (a[2], b[2], af.t);
    shelf (lo, f * v[0], v[1], v[2], sr, out);
    return true;
}

double tableAt (const float* xs, const float* ys, int n, double x) noexcept
{
    const auto a = nodeAt (xs, n, x, true);
    return std::exp (lerp (std::log ((double) ys[a.i]), std::log ((double) ys[a.i + 1]), a.t));
}

bool highPass (const Settings& s, double sr, Biquad& first, Biquad& second) noexcept
{
    if (s.hpHz < 16.5)
        return false;
    const double f = std::clamp (s.hpHz, 20.0, 350.0);
    const double f0 = tableAt (hpDial, hpF0, 5, f), q = lerp (hpQ[0], hpQ[4], (f - 20.0) / 330.0);
    first.set (1.0, 0.0, 0.0, 1.0, 0.0, 0.0);
    const double n[3] { 1.0, 0.0, 0.0 }, d[3] { 1.0, 1.0 / q, 1.0 };
    bilinearAt (n, d, f0, sr, second);
    return true;
}

bool lowPass (const Settings& s, double sr, Biquad& out) noexcept
{
    if (s.lpKHz <= 0.0 || s.lpKHz >= 15.9)
        return false;
    const double f = std::clamp (s.lpKHz, 3.0, 15.95);
    const double f0 = tableAt (lpDial, lpF0, 5, f);
    const double n[3] { 0.0, 0.0, 1.0 }, d[3] { 1.0, 1.0 / 0.716, 1.0 };
    bilinearAt (n, d, f0, sr, out);
    return true;
}
} // namespace k4
} // namespace

double qFromDial (double v) noexcept
{
    v = std::clamp (v, 0.0, 1.0) * 20.0;
    const int i = std::min ((int) v, 19);
    return qText[i] + (v - i) * (qText[i + 1] - qText[i]);
}

double dialFromQ (double q) noexcept
{
    q = std::clamp (q, (double) qText[20], (double) qText[0]);
    for (int i = 0; i < 20; ++i)
        if (q <= qText[i] && q >= qText[i + 1])
            return (i + (qText[i] - q) / (qText[i] - qText[i + 1])) / 20.0;
    return 1.0;
}

bool bandBiquad (int band, const Settings& s, double sampleRate, Biquad& out)
{
    if (s.model == 1)
        return k4::band (band, s, sampleRate, out);
    double n[3], d[2], wref = 0.0;
    if (! bandAnalog (band, s, n, d, wref))
        return false;
    const double a = rateScale (wref, sampleRate);
    n[1] *= a; n[2] *= a * a; d[0] *= a; d[1] *= a * a;
    bilinear (n, d, 2.0 * sampleRate, out);
    return true;
}

bool highPass (const Settings& s, double sampleRate, Biquad& first, Biquad& second)
{
    if (s.model == 1)
        return k4::highPass (s, sampleRate, first, second);
    if (s.hpHz < 16.5)
        return false;
    // 3rd-order Butterworth in the analog domain (a real pole and a Q = 1 pair at the same frequency), measured
    double w = interp (hpText, hpW, std::size (hpText), roundTo (std::clamp (s.hpHz, 17.0, 350.0), 1.0));
    w *= rateScale (w, sampleRate);
    const double K = 2.0 * sampleRate;
    // first order s / (s + w)
    first.set (K, -K, 0.0, K + w, w - K, 0.0);
    const double n[3] = { 1.0, 0.0, 0.0 }, dd[2] = { w, w * w };
    bilinear (n, dd, K, second);
    return true;
}

bool lowPass (const Settings& s, double sampleRate, Biquad& out)
{
    if (s.model == 1)
        return k4::lowPass (s, sampleRate, out);
    if (s.lpKHz <= 0.0)
        return false;
    const double f = roundTo (std::clamp (s.lpKHz, 3.0, 19.5), 0.1) * 1000.0;
    // the table holds (n2, n1, n0, d1, d0) per displayed frequency
    constexpr auto count = std::size (lpText);
    std::size_t i = 0;
    while (i < count - 2 && f > lpText[i + 1])
        ++i;
    const double t = std::clamp ((f - lpText[i]) / (lpText[i + 1] - lpText[i]), 0.0, 1.0);
    double c[5];
    for (int k = 0; k < 5; ++k)
        c[k] = (1 - t) * lpCoef[i * 5 + k] + t * lpCoef[(i + 1) * 5 + k];
    double n[3] = { c[0], c[1], c[2] }, d[2] = { c[3], c[4] };
    const double a = rateScale (std::sqrt (std::max (d[1], 1.0)), sampleRate);
    n[1] *= a; n[2] *= a * a; d[0] *= a; d[1] *= a * a;
    bilinear (n, d, 2.0 * sampleRate, out);
    return true;
}

double responseDb (const Settings& s, double sampleRate, double hz)
{
    const double w = 2.0 * pi * hz / sampleRate;
    double m = 1.0;
    Biquad a, b;
    if (highPass (s, sampleRate, a, b))
        m *= biquadMagnitude (a, w) * biquadMagnitude (b, w);
    if (lowPass (s, sampleRate, a))
        m *= biquadMagnitude (a, w);
    if (s.eqOn)
        for (int band = 0; band < 4; ++band)
            if (bandBiquad (band, s, sampleRate, a))
                m *= biquadMagnitude (a, w);
    return gainToDb (m);
}

// ---- dynamics ----
namespace
{
#include "core/VoiceMatchDynamics.inc"

double softplus (double o, double k) noexcept { return o > 30.0 * k ? o : k * std::log1p (std::exp (o / k)); }

// ---- round 148: the "4K" console's dynamics, in the same feedback model ([EXEC] SSL 4K E trial; REA_rodada148 §2)
#include "core/Console4KDynamics.inc"

double rowInterp (const double* xs, int n, const double* rows, int stride, int k, double x) noexcept
{
    if (x <= xs[0]) return rows[k];
    for (int i = 0; i < n - 1; ++i)
        if (x <= xs[i + 1])
        {
            const double t = (x - xs[i]) / (xs[i + 1] - xs[i]);
            return rows[i * stride + k] + t * (rows[(i + 1) * stride + k] - rows[i * stride + k]);
        }
    return rows[(n - 1) * stride + k];
}

CompLaw k4CompLaw (const Settings& s) noexcept
{
    CompLaw c {};
    const double r = std::max (1.0, s.compRatio);
    const double u = r > 1e6 ? 1.0 : 1.0 - 1.0 / r;
    const double thr = std::clamp (s.compThreshold, -20.0, 10.0);
    constexpr int nT = (int) std::size (k4Thr), nU = (int) std::size (k4U);
    int ti = 0;
    while (ti < nT - 2 && thr < k4Thr[ti + 1]) ++ti;
    const double tt = std::clamp ((k4Thr[ti] - thr) / (k4Thr[ti] - k4Thr[ti + 1]), 0.0, 1.0);
    const double uu = std::max (u, k4U[0]);
    int ui = 0;
    while (ui < nU - 2 && uu > k4U[ui + 1]) ++ui;
    const double tu = std::clamp ((uu - k4U[ui]) / (k4U[ui + 1] - k4U[ui]), 0.0, 1.0);
    auto bil = [&] (const double* g, int stride, int k) {
        auto at = [&] (int a, int b) { return g[(a * nU + b) * stride + k]; };
        return (1 - tt) * ((1 - tu) * at (ti, ui) + tu * at (ti, ui + 1)) + tt * ((1 - tu) * at (ti + 1, ui) + tu * at (ti + 1, ui + 1));
    };
    // below the lowest measured ratio (1.8) the slope and make-up fade to 0 at 1:1
    const double fade = u < k4U[0] ? u / k4U[0] : 1.0;
    c.thresholdOffset = bil (k4Static, 3, 0);
    c.knee = std::max (0.2, bil (k4Static, 3, 1));
    c.slope = bil (k4Static, 3, 2) * fade;
    // the automatic make-up (measured at -60 dBFS) and its offset: [EXEC] +-6 dB of offset moves the make-up by
    // 1.15 / 1.37 / 1.88 dB at 2.9:1 / 5:1 / inf:1 (never below 0)
    double mk = bil (k4Makeup, 1, 0) * fade;
    const double per = u <= 0.655 ? 0.192 * u / 0.655 : (u <= 0.8 ? 0.192 + (u - 0.655) / 0.145 * 0.036 : 0.228 + (u - 0.8) / 0.2 * 0.085);
    mk = std::max (0.0, mk + per * std::clamp (s.makeupOffsetDb, -12.0, 12.0));
    c.makeup = s.autoMakeup ? mk : 0.0;
    // attack: fast = instant (the fit's floor, 0.05 ms); otherwise the console's own time, longer for deeper settings
    c.attackFastMs = 0.05;
    c.attackSlowMs = std::exp (k4AttackLaw[0] + k4AttackLaw[1] * u + k4AttackLaw[2] * thr);
    const double rel = std::clamp (s.compRelease, 0.1, 4.0);
    const double* knobs = s.compFast ? k4RelKnobFast : k4RelKnobSlow;
    const double* rows = s.compFast ? k4RelFast : k4RelSlow;
    const int nk = s.compFast ? (int) std::size (k4RelKnobFast) : (int) std::size (k4RelKnobSlow);
    c.release1Fraction = rowInterp (knobs, nk, rows, 3, 0, rel);
    c.release1Ms = rowInterp (knobs, nk, rows, 3, 1, rel);
    c.releaseMs = rowInterp (knobs, nk, rows, 3, 2, rel);
    c.detectorAttackMs = 0.021;
    c.detectorReleaseMs = 0.064;
    return c;
}

// [EXEC] the 4K's expander / gate on 1 kHz stairs and bursts: threshold about 23.3 dB under the console's scale, the
// expander 1.13 dB of reduction per dB below it, the range dial's depth 5 / 10 / 20 / 40 -> 7.8 / 17.1 / 26.7 / 47.3 dB,
// opening within half a millisecond, closing over 117 ms x (release / 0.1 s)^0.58 (gate) and 21 ms x (release / 0.1 s)^0.6
// (expander); the gate's hysteresis was not measured (round 123's 3.9 dB kept) [model]
GateLaw k4GateLaw (const Settings& s) noexcept
{
    GateLaw g {};
    static constexpr double dial[5] { 0.0, 5.0, 10.0, 20.0, 40.0 }, depth[5] { 0.0, 7.8, 17.1, 26.7, 47.3 };
    g.range = knobInterp (dial, depth, 5, std::clamp (s.expRange, 0.0, 40.0));
    g.threshold = std::clamp (s.expThreshold, -30.0, 10.0) - 23.3;
    g.attackMs = s.expFast ? 0.05 : 0.3;
    const double rel = std::clamp (s.expRelease, 0.1, 4.0) / 0.1;
    g.releaseMs = s.gate ? 117.0 * std::pow (rel, 0.58) : 21.0 * std::pow (rel, 0.6);
    g.detectorReleaseMs = 50.92;
    g.slope = 1.13;
    g.knee = 0.297;
    g.hysteresis = 3.859;
    g.gate = s.gate;
    return g;
}

CompLaw lawOf (const Settings& s) noexcept { return s.model == 1 ? k4CompLaw (s) : compLaw (s); }
GateLaw gateOf (const Settings& s) noexcept { return s.model == 1 ? k4GateLaw (s) : gateLaw (s); }

// the 4K's mic pre ([EXEC] 1 kHz / 100 Hz stairs at Mic 0 … 50 dB, level-compensated by the console): the drive is
// 0.744 dB per Mic dB into a curve that first expands a little (+0.5 … 0.8 dB just before it saturates) and then clips
// at 3.9 (+11.8 dB over full scale at the curve's input), with a slight asymmetry (H2 −40 … −75 dBc); Pre In alone −0.27 dB
double micPre (double x, double drive, double inv) noexcept
{
    constexpr double c = 3.901, p = 23.49, b = 0.0264, a = 0.0230;
    auto curve = [] (double u) { const double v = u + a * u * u * u; return v / std::pow (1.0 + std::pow (std::abs (v / c), p), 1.0 / p); };
    static const double off = curve (b);
    return 0.96939 * (curve (x * drive + b) - off) * inv; // 0.96939 = −0.27 dB
}

// the Analogue VCA ([EXEC] 1 kHz at −30 … −6 dBFS: H2 −77 … −57 dBc, H3 −107 … −66 dBc, level unchanged): x + 0.0089 x² +
// 0.018 x³, the terms easing off above −10 dBFS
double vcaColour (double x) noexcept
{
    const double e = 1.0 / (1.0 + 2.5 * x * x);
    return x + (0.0089 * x * x + 0.018 * x * x * x) * e;
}
} // namespace

double compStaticGainDb (const Settings& s, double sineDbfs)
{
    // iterate the feedback loop to its fixed point for a steady sine (peak detector on the output)
    const auto law = lawOf (s);
    double gr = 0.0;
    for (int it = 0; it < 200; ++it)
    {
        const double o = sineDbfs - gr - (s.compThreshold + law.thresholdOffset);
        const double target = law.slope * softplus (o, law.knee);
        gr += 0.3 * (target - gr);
    }
    return law.makeup - gr;
}

double compMakeupDb (const Settings& s) noexcept
{
    return s.compRatio > 1.0 ? lawOf (s).makeup : 0.0;
}

void Channel::prepare (double sampleRate) noexcept
{
    sr = sampleRate;
    vcaHp.filter (sr, 1, 3.0, 0.7071); // round 148: the 4K VCA's coupling high-pass
    designedOnce = false;
    reset();
}

void Channel::reset() noexcept
{
    hp1.reset(); hp2.reset(); lp.reset(); vcaHp.reset();
    for (auto& b : bands)
        b.reset();
    for (auto& c : sc)
    {
        c.hp1.reset(); c.hp2.reset(); c.lp.reset();
        for (auto& b : c.bands)
            b.reset();
    }
    env = gr = gr1 = yl = yr = genv = ggr = 0.0;
    gateOpen = false;
}

void Channel::design (const Settings& s) noexcept
{
    const auto eq = [] (const Settings& a, const Settings& b) {
        return a.hpHz == b.hpHz && a.lpKHz == b.lpKHz && a.lfGain == b.lfGain && a.lfHz == b.lfHz && a.lfBell == b.lfBell
            && a.lmfGain == b.lmfGain && a.lmfKHz == b.lmfKHz && a.lmfQ == b.lmfQ && a.hmfGain == b.hmfGain && a.hmfKHz == b.hmfKHz
            && a.hmfQ == b.hmfQ && a.hfGain == b.hfGain && a.hfKHz == b.hfKHz && a.hfBell == b.hfBell
            && a.model == b.model && a.colour == b.colour;
    };
    if (designedOnce && eq (s, designed))
        return;
    designed = s;
    designedOnce = true;
    Biquad a, b;
    hpOn = highPass (s, sr, a, b);
    if (hpOn) { hp1.b0 = a.b0; hp1.b1 = a.b1; hp1.b2 = a.b2; hp1.a1 = a.a1; hp1.a2 = a.a2; hp2.b0 = b.b0; hp2.b1 = b.b1; hp2.b2 = b.b2; hp2.a1 = b.a1; hp2.a2 = b.a2; }
    lpOn = lowPass (s, sr, a);
    if (lpOn) { lp.b0 = a.b0; lp.b1 = a.b1; lp.b2 = a.b2; lp.a1 = a.a1; lp.a2 = a.a2; }
    for (int i = 0; i < 4; ++i)
    {
        bandOn[i] = bandBiquad (i, s, sr, a);
        if (bandOn[i]) { bands[i].b0 = a.b0; bands[i].b1 = a.b1; bands[i].b2 = a.b2; bands[i].a1 = a.a1; bands[i].a2 = a.a2; }
    }
    const auto coefs = [] (Biquad& to, const Biquad& from) { to.b0 = from.b0; to.b1 = from.b1; to.b2 = from.b2; to.a1 = from.a1; to.a2 = from.a2; };
    for (auto& c : sc) // round 165: the side chain's copies
    {
        coefs (c.hp1, hp1); coefs (c.hp2, hp2); coefs (c.lp, lp);
        for (int i = 0; i < 4; ++i)
            coefs (c.bands[i], bands[i]);
    }
}

void Channel::process (float* l, float* r, int n, const Settings& s) noexcept
{
    design (s);
    const double inG = dbToGain (roundTo (s.inputDb, 0.1)), outG = dbToGain (roundTo (s.outputDb, 0.1)) * (s.phase ? -1.0 : 1.0);
    const auto law = lawOf (s);
    const bool compOn = s.dynOn && s.compRatio > 1.0005;
    const double cA = coefMs (s.compFast ? law.attackFastMs : law.attackSlowMs, sr), cR = coefMs (law.releaseMs, sr), cR1 = coefMs (law.release1Ms, sr);
    const double frac = law.release1Fraction;
    const double dA = coefMs (law.detectorAttackMs, sr), dR = coefMs (law.detectorReleaseMs, sr);
    const double makeup = s.dynOn ? dbToGain (law.makeup) : 1.0;
    const auto gl = gateOf (s);
    const bool gateOn = s.dynOn && gl.range > 0.05;
    const double gA = coefMs (gl.attackMs, sr), gR = coefMs (gl.releaseMs, sr), gD = coefMs (gl.detectorReleaseMs, sr);
    float maxComp = 0.0f, maxGate = 0.0f;
    // round 148: the 4K's mic pre and compressor mix
    const bool micOn = s.model == 1 && s.pre;
    const double micDrive = dbToGain (0.744 * std::clamp (s.micDb, 0.0, 50.0)), micInv = 1.0 / micDrive;
    const double mix = std::clamp (s.compMix, 0.0, 1.0);
    const bool mixed = s.model == 1 && mix < 0.9995;
    // [EXEC] the 4K's hiss between impulses: -106.6 dBFS rms with the Analogue VCA, -104.2 dBFS with the mic pre in
    // (white, the two channels independent [model]), before the output trim
    const double hiss = s.model != 1 ? 0.0 : std::sqrt ((s.vca ? 2.188e-11 : 0.0) + (s.pre ? 3.802e-11 : 0.0));
    auto gauss = [] (std::uint32_t& st) noexcept {
        double sum = 0.0;
        for (int k = 0; k < 4; ++k)
        {
            st ^= st << 13; st ^= st >> 17; st ^= st << 5;
            sum += (double) (st >> 8) * (1.0 / 16777216.0);
        }
        return (sum - 2.0) * 1.7320508;
    };

    auto filters = [this] (double& a, double& b) {
        if (hpOn)
        {
            a = hp2.process (hp1.process ((float) a, 0), 0);
            b = hp2.process (hp1.process ((float) b, 1), 1);
        }
        if (lpOn)
        {
            a = lp.process ((float) a, 0);
            b = lp.process ((float) b, 1);
        }
    };
    auto eq = [this] (double& a, double& b) {
        for (int i = 0; i < 4; ++i)
            if (bandOn[i])
            {
                a = bands[i].process ((float) a, 0);
                b = bands[i].process ((float) b, 1);
            }
    };

    // round 165: EQ to the dynamics' side chain (model 0) — the detectors hear the filtered / equalised signal, the
    // audio goes through the dynamics without them (the filters stay at the input when split)
    const bool eqSc = s.eqToSidechain && s.model == 0;
    auto sideChain = [this, &s] (ScChain& c, double a, double b) {
        if (! s.filtersBeforeDynamics)
        {
            if (hpOn) { a = c.hp2.process (c.hp1.process ((float) a, 0), 0); b = c.hp2.process (c.hp1.process ((float) b, 1), 1); }
            if (lpOn) { a = c.lp.process ((float) a, 0); b = c.lp.process ((float) b, 1); }
        }
        if (s.eqOn)
            for (int k = 0; k < 4; ++k)
                if (bandOn[k]) { a = c.bands[k].process ((float) a, 0); b = c.bands[k].process ((float) b, 1); }
        return std::max (std::abs (a), std::abs (b));
    };

    for (int i = 0; i < n; ++i)
    {
        double a = l[i], b = r[i];
        if (micOn) // the 4K's mic pre sees the input before its trim ([EXEC] the drive does not follow the trim)
        {
            a = micPre (a, micDrive, micInv);
            b = micPre (b, micDrive, micInv);
        }
        a *= inG;
        b *= inG;
        if (eqSc ? s.filtersBeforeDynamics : (s.filtersBeforeDynamics || s.dynamicsAfterEq))
            filters (a, b);
        if (! eqSc && s.dynamicsAfterEq && s.eqOn)
            eq (a, b);

        if (gateOn)
        {
            const double lev = eqSc ? sideChain (sc[0], a, b) : std::max (std::abs (a), std::abs (b));
            genv = lev > genv ? lev : genv + gD * (lev - genv);
            const double levDb = gainToDb (genv);
            double target;
            if (gl.gate)
            {
                if (levDb >= gl.threshold) gateOpen = true;
                else if (levDb < gl.threshold - gl.hysteresis) gateOpen = false;
                target = gateOpen ? 0.0 : gl.range;
            }
            else
            {
                const double u = gl.threshold - levDb;
                target = std::min (gl.range, gl.slope * (u > 30.0 * gl.knee ? u : gl.knee * std::log1p (std::exp (u / gl.knee))));
            }
            ggr += (target < ggr ? gA : gR) * (target - ggr); // less reduction = opening (attack), more = closing (release)
            const double g = dbToGain (-ggr);
            a *= g;
            b *= g;
            maxGate = std::max (maxGate, (float) ggr);
        }
        if (compOn)
        {
            // feedback compressor: the detector hears the compressor's own output (the previous sample)
            const double det = eqSc ? sideChain (sc[1], yl, yr) : std::max (std::abs (yl), std::abs (yr));
            env += (det > env ? dA : dR) * (det - env);
            const double o = gainToDb (env) - (s.compThreshold + law.thresholdOffset);
            const double target = law.slope * softplus (o, law.knee);
            gr1 += (target > gr1 ? cA : cR1) * (target - gr1);
            gr += (target > gr ? cA : cR) * (target - gr);
            const double grNow = frac * gr1 + (1.0 - frac) * gr;
            const double g = dbToGain (-grNow);
            yl = a * g;
            yr = b * g;
            if (mixed) // round 148: the 4K's mix blends the reduction in dB ([EXEC] 50 % = half the dB at every level)
            {
                const double gm = dbToGain (-grNow * mix);
                a *= gm;
                b *= gm;
            }
            else
            {
                a = yl;
                b = yr;
            }
            maxComp = std::max (maxComp, (float) (frac * gr1 + (1.0 - frac) * gr));
        }
        if (s.model == 1 && s.vca)
        {
            // [EXEC] pink noise through the VCA: a coupling high-pass (2nd order, ~3 Hz: 25 deg at 10 Hz, -2.8 dB at 3 Hz)
            a = vcaHp.process ((float) vcaColour (a), 0);
            b = vcaHp.process ((float) vcaColour (b), 1);
        }
        a *= makeup;
        b *= makeup;

        if (! s.dynamicsAfterEq && ! eqSc)
        {
            if (! s.filtersBeforeDynamics)
                filters (a, b);
            if (s.eqOn)
                eq (a, b);
        }
        if (hiss > 0.0)
        {
            a += hiss * gauss (noise[0]);
            b += hiss * gauss (noise[1]);
        }
        l[i] = (float) (a * outG);
        r[i] = (float) (b * outG);
    }
    lastComp = maxComp;
    lastGate = maxGate;
}
} // namespace echannel

// ================================================================== split-band de-esser
// [EXEC] Fitted on the official DeEsser trial (her state and sweeps of threshold -40 .. -5 dB and frequency 2.5 .. 14.4
// kHz; 6 kHz sine stairs, pink noise, her takes; tools/fxmeasure/py/vm123/decfg.py): the side chain is a 4th-order
// high-pass (two Butterworth sections) or a band-pass at the dial's frequency, an energy (power) detector with a
// 0.3 ms attack and a 12 ms release, a 7.1 : 1 soft-knee (1.26 dB) gain computer 0.6 dB below the dial; Split
// attenuates only what lies above a first-order crossover at 0.33 x the dial's frequency (so the attenuation of a
// sibilant floors near -11 dB: no range control), Wideband the whole signal. Her state: null -38 / -50 / -44 dB.
namespace deesser
{
namespace
{
constexpr double thrOffset = -0.6009, thrSlope = -0.0097, ratio = 7.1171, knee = 1.2597, attackMs = 0.3027, releaseMs = 12.1503;
constexpr double splitK = 0.3306, splitExp = 0.0491, scK = 1.0120, scExp = -0.0353;
// round 165 [EXEC] (DeEsser 17.1 trial, tone stairs, thresholds −10 … −60): the reduction stops at 25.6 dB (wide band);
// BandPass is the HighPass side chain followed by a 4th-order low-pass (two 2nd-order sections, Q 0.68) at 1.8435 x the
// frequency (x (f / 5506)^-0.139: 1.75 x at 8 kHz: 9 … 14 kHz within 0.2 dB, 2 dB at 16 kHz): the trial's BandPass needs 0.3 / 0.7 / 1.8 / 4.4 / 11.9 dB more than its HighPass at 4.5 / 5.5 / 7 / 9 /
// 12 kHz (dial 5506 Hz), this low-pass 0.06 dB rms from that (round 123's band-pass, Q 1.3 + 3.9 dB, was up to 8 dB off,
// and the translator read every BandPass state as HighPass)
constexpr double maxReductionDb = 25.6, bpLpK = 1.8435, bpLpExp = -0.139, bpLpQ = 0.68;

void rbj (Biquad& b, int type, double f, double q, double sr) noexcept
{
    const double w = 2.0 * pi * std::min (f, sr * 0.49) / sr, cw = std::cos (w), al = std::sin (w) / (2.0 * q);
    if (type == 0) b.set ((1 + cw) / 2, -(1 + cw), (1 + cw) / 2, 1 + al, -2 * cw, 1 - al); // high-pass
    else if (type == 2) b.set ((1 - cw) / 2, 1 - cw, (1 - cw) / 2, 1 + al, -2 * cw, 1 - al); // low-pass
    else b.set (al, 0.0, -al, 1 + al, -2 * cw, 1 - al);                                     // band-pass (0 dB peak)
}
} // namespace

void DeEsser::prepare (double sampleRate) noexcept
{
    sr = sampleRate;
    designedHz = -1.0;
    reset();
}

void DeEsser::reset() noexcept
{
    sc1.reset();
    sc2.reset();
    bp1.reset();
    bp2.reset();
    lpX[0] = lpX[1] = lpY[0] = lpY[1] = 0.0;
    env = gr = 0.0;
}

void DeEsser::design (const Settings& s) noexcept
{
    const double f = std::clamp (s.hz, 2000.0, 16000.0);
    if (f == designedHz && s.bandPass == designedBp)
        return;
    designedHz = f;
    designedBp = s.bandPass;
    const double rel = f / 5506.0;
    const double fsc = f * scK * std::pow (rel, scExp);
    rbj (sc1, 0, fsc, 0.70710678, sr);
    sc2.b0 = sc1.b0; sc2.b1 = sc1.b1; sc2.b2 = sc1.b2; sc2.a1 = sc1.a1; sc2.a2 = sc1.a2;
    rbj (bp1, 2, f * bpLpK * std::pow (rel, bpLpExp), bpLpQ, sr);
    bp2.b0 = bp1.b0; bp2.b1 = bp1.b1; bp2.b2 = bp1.b2; bp2.a1 = bp1.a1; bp2.a2 = bp1.a2;
    const double k = std::tan (pi * std::min (f * splitK * std::pow (rel, splitExp), 0.45 * sr) / sr);
    lpB = k / (1.0 + k);
    lpA = (k - 1.0) / (k + 1.0);
}

void DeEsser::process (float* l, float* r, int n, const Settings& s) noexcept
{
    design (s);
    const double T = std::clamp (s.thresholdDb, -80.0, 0.0);
    const double thr = T + thrOffset + thrSlope * (T + 24.2);
    const double aA = coefMs (attackMs, sr), aR = coefMs (releaseMs, sr), out = dbToGain (s.outputDb);
    double maxGr = 0.0, maxO = -200.0;
    for (int i = 0; i < n; ++i)
    {
        double a = sc2.process (sc1.process (l[i], 0), 0), b = sc2.process (sc1.process (r[i], 1), 1);
        if (s.bandPass)
        {
            a = bp2.process (bp1.process ((float) a, 0), 0);
            b = bp2.process (bp1.process ((float) b, 1), 1);
        }
        const double p = 0.5 * (a * a + b * b);
        env += (p > env ? aA : aR) * (p - env);
        const double o = 10.0 * std::log10 (std::max (env, 1e-24)) - thr;
        maxO = std::max (maxO, o);
        gr = std::min (maxReductionDb, (1.0 - 1.0 / ratio) * (o > 30.0 * knee ? o : knee * std::log1p (std::exp (o / knee))));
        const double g = dbToGain (-gr);
        maxGr = std::max (maxGr, gr);
        if (s.listen)
        {
            l[i] = (float) (a * out);
            r[i] = (float) (b * out);
            continue;
        }
        double x[2] = { l[i], r[i] };
        for (int c = 0; c < 2; ++c)
        {
            if (s.wideband)
                x[c] *= g;
            else
            {
                const double lo = lpB * (x[c] + lpX[c]) - lpA * lpY[c];
                lpX[c] = x[c];
                lpY[c] = lo;
                x[c] = lo + (x[c] - lo) * g;
            }
        }
        l[i] = (float) (x[0] * out);
        r[i] = (float) (x[1] * out);
    }
    last = (float) maxGr;
    lastDet = (float) (maxO + T); // where the energy sits against the threshold dial: reduction starts at T
}
} // namespace deesser

// ================================================================== round 148: single-vocal de-esser
// [EXEC] FabFilter Pro-DS, Single Vocal, wide band, her two states (threshold -43.7 / -36 dB, range 6 dB, look-ahead
// 12 ms, band 7 … 14 kHz): latency 720 samples at 48 kHz (15 ms); the static law GR = range (L - thr + 4.1) /
// (0.1 - thr) from thr + 3.9 dB on, hysteresis down to thr - 4.1 dB; the band 4th-order high-pass x 4th-order low-pass;
// pink noise never triggers it, a steady bright tone does unless a lower tone was already there. The relative test,
// the detector and gain times and the look-ahead were fitted to its gain envelopes on 8 kHz steps and her three takes
// (tmp/vm124x/pds: pds_fit.py, proto_ds.cpp; 0.06–0.19 dB RMS over all 2 ms frames of her takes). Times in ms.
namespace vocalds
{
namespace
{
constexpr double detAttackMs = 0.01, detReleaseMs = 1.71, gainAttackMs = 2.95, gainReleaseMs = 1.765;
constexpr double extraAheadMs = 0.08, ratioMs = 0.2683, openRatioDb = -7.054, closeRatioDb = -2.263;
constexpr double openDb = 3.9, closeDb = -4.1;
double pole (double ms, double sr) noexcept { return ms <= 0.0 ? 0.0 : std::exp (-1.0 / (ms * 0.001 * sr)); }
} // namespace

void DeEsser::prepare (double sampleRate) noexcept
{
    sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    latency = std::clamp ((int) std::lround (0.015 * sr), 1, ring / 2);
    designedHp = designedLp = -1.0;
    reset();
}

void DeEsser::reset() noexcept
{
    for (auto* b : { &hp1, &hp2, &lp1, &lp2, &low1, &low2, &split1, &split2 })
        b->reset();
    eh = qh = ql = sm = 0.0;
    open = false;
    dl.fill (0.0f);
    dr.fill (0.0f);
    level.fill (-300.0f);
    ratio.fill (-300.0f);
    pos = 0;
    last = 0.0f;
}

void DeEsser::design (const Settings& s) noexcept
{
    const double h = std::clamp (s.hpHz, 1000.0, sr * 0.45), lo = std::clamp (s.lpHz, h, sr * 0.45);
    if (h == designedHp && lo == designedLp)
        return;
    designedHp = h;
    designedLp = lo;
    // 4th-order Butterworth sections (Q 0.5412 / 1.3066)
    hp1.filter (sr, 1, h, 0.54119610); hp2.filter (sr, 1, h, 1.30656296);
    lp1.filter (sr, 0, lo, 0.54119610); lp2.filter (sr, 0, lo, 1.30656296);
    low1.filter (sr, 0, h, 0.54119610); low2.filter (sr, 0, h, 1.30656296);
    split1.filter (sr, 0, h, 0.70710678); split2 = split1;
}

void DeEsser::process (float* l, float* r, int n, const Settings& s) noexcept
{
    design (s);
    const double ca = pole (detAttackMs, sr), cr = pole (detReleaseMs, sr), ga = pole (gainAttackMs, sr), gr = pole (gainReleaseMs, sr);
    const double cq = pole (ratioMs, sr);
    const double thr = std::clamp (s.thresholdDb, -60.0, 0.0), range = std::clamp (s.rangeDb, 0.0, 24.0);
    const int ahead = std::clamp ((int) std::lround ((s.lookahead ? std::clamp (s.lookaheadMs, 0.0, 15.0) + extraAheadMs : 0.0) * 0.001 * sr), 0, latency);
    constexpr std::int64_t m = ring - 1;
    float maxGr = 0.0f, maxDet = -300.0f;
    for (int i = 0; i < n; ++i)
    {
        const float a = l[i], b = r[i];
        const double x = 0.5 * ((double) a + (double) b); // the mid signal (her states: stereo link 100 %, mid)
        const double h = lp2.process (lp1.process (hp2.process (hp1.process ((float) x, 0), 0), 0), 0);
        const double lw = low2.process (low1.process ((float) x, 0), 0);
        const double ph = h * h, pl = lw * lw;
        eh = ph > eh ? ca * eh + (1.0 - ca) * ph : cr * eh + (1.0 - cr) * ph;
        qh = cq * qh + (1.0 - cq) * ph;
        ql = cq * ql + (1.0 - cq) * pl;
        const auto w = (std::size_t) (pos & m);
        level[w] = (float) (10.0 * std::log10 (2.0 * eh + 1e-30));
        ratio[w] = (float) (10.0 * std::log10 ((qh + 1e-30) / (ql + 1e-30)));
        dl[w] = a;
        dr[w] = b;
        // the sample played now entered `latency` samples ago; its gain looks `ahead` samples past it
        const auto now = pos - latency;
        double gainDb = 0.0;
        if (now >= 0)
        {
            const double la = level[(std::size_t) ((now + ahead) & m)], rr = ratio[(std::size_t) (now & m)];
            if (! open && la >= thr + openDb && rr >= openRatioDb)
                open = true;
            else if (open && (la < thr + closeDb || rr < closeRatioDb))
                open = false;
            const double t = open ? std::clamp (range * (la - thr + 4.1) / (0.1 - thr), 0.0, range) : 0.0;
            sm = t > sm ? ga * sm + (1.0 - ga) * t : gr * sm + (1.0 - gr) * t;
            gainDb = -sm;
            maxDet = std::max (maxDet, (float) la);
        }
        const auto rd = (std::size_t) (std::max<std::int64_t> (now, 0) & m);
        const float da = now >= 0 ? dl[rd] : 0.0f, db = now >= 0 ? dr[rd] : 0.0f;
        const auto g = (float) std::pow (10.0, gainDb / 20.0);
        if (s.listen)
        {
            l[i] = lp2.process (lp1.process (hp2.process (hp1.process (da, 1), 1), 1), 1); // (the band, on the spare state)
            r[i] = l[i];
        }
        else if (s.wideband)
        {
            l[i] = da * g;
            r[i] = db * g;
        }
        else
        {
            // split [model]: only the part above the high-pass is turned down (complementary 2nd-order split)
            const auto la2 = split1.process (da, 0), lb2 = split2.process (db, 1);
            l[i] = la2 + (da - la2) * g;
            r[i] = lb2 + (db - lb2) * g;
        }
        maxGr = std::max (maxGr, (float) sm);
        ++pos;
    }
    last = maxGr;
    lastDet = maxDet;
}
} // namespace vocalds

// ================================================================== electro-optical leveler
// [EXEC] Fitted on the official CLA-2A trial (tools/fxmeasure/py/vm123: sine stairs at 100 Hz … 10 kHz, 1 kHz bursts,
// a 1 kHz tone with a quiet pink probe at five levels, pink noise, her takes; dynfit "cla5"). A feedback leveler: the
// side chain hears the cell's output through a peaking emphasis (+9.4 dB at 9 kHz); the cell's conductance C follows
// the light — C = ((level - Yt) / Y0)^gamma above a threshold — with an attack in the log domain that speeds up with C,
// a hyperbolic release (dC/dt = -a C^n: about half the reduction back in 60 ms, the rest over seconds) and a slow
// second conductance; the audio sees g = 1 / (1 + C) with a high-frequency bypass that opens under reduction (the
// measured tilt: up to +7 dB at 20 kHz), then the gain dial's make-up, a little 2nd / 3rd harmonic, and the unit's
// idle response (a 4 Hz high-pass and a 0.7 dB dip around 12 kHz, measured).
namespace opto
{
namespace
{
#include "core/VoiceMatchOpto.inc"
} // namespace

void Leveler::prepare (double sampleRate) noexcept
{
    sr = sampleRate;
    reset();
}

void Leveler::reset() noexcept
{
    cf = cs = el = mem = 0.0;
    for (int c = 0; c < 2; ++c)
    {
        yc[c] = fx1[c] = fy1[c] = xp[c] = 0.0;
        for (int k = 0; k < 4; ++k)
            tx[c][k] = ty[c][k] = 0.0;
    }
    sc.reset();
}

void Leveler::process (float* l, float* r, int n, const Settings& s) noexcept
{
    const double dt = 1.0 / sr;
    if (scDesignedRate != sr)
    {
        scDesignedRate = sr;
        const double A = std::pow (10.0, scDb / 40.0), w = 2.0 * pi * std::min (scHz, 0.45 * sr) / sr, al = std::sin (w) / (2.0 * scQ), cw = std::cos (w);
        sc.set (1 + al * A, -2 * cw, 1 - al * A, 1 + al / A, -2 * cw, 1 - al / A);
    }
    const bool ml = s.limit && s.measured; // round 152: the measured Limit law
    const double shift = sensitivityDb (s.peakReduction, s.hiFreq) + (s.limit ? (ml ? limitShiftDb152 : limitShiftDb) : 0.0);
    const double Y0 = dbToGain (y0Db + shift), Yt = dbToGain (ytDb + shift), gam = s.limit ? (ml ? limitGamma152 : limitGamma) : gamma;
    const double ff = ml ? limitFeedForward : 0.0, gFloor = ml ? dbToGain (-limitFloorDb) : 0.0;
    const double mk = dbToGain (makeupDb (s.gain) + idleMakeupDb + (s.measured && s.autoMakeup ? autoMakeupDb (s.peakReduction) : 0.0));
    const double cA = 1.0 - std::exp (-dt * 1000.0 / attackSlowMs), mUp = 1.0 - std::exp (-dt * 1000.0 / memUpMs), mDn = 1.0 - std::exp (-dt * 1000.0 / memDownMs);
    const double K = 2.0 * sr;
    // the idle response measured at 48 kHz; other rates take its bilinear analog prototype (approximate)
    const double* tb = idleB;
    const double* ta = idleA;
    double maxGr = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double e0 = sc.process ((float) (yc[0] + ff * xp[0]), 0), e1 = sc.process ((float) (yc[1] + ff * xp[1]), 1);
        el = std::max (std::abs (e0), std::abs (e1)); // the light follows the drive within the sample (fitted)
        const double ct = (el > Yt ? std::pow ((el - Yt) / Y0, gam) : 0.0) * (1.0 + memGain * mem);
        const double ctf = (1.0 - slowShare) * ct, cts = slowShare * ct;
        if (ctf > cf)
        {
            double cd = std::log1p (cf);
            cd += (std::log1p (ctf) - cd) * (1.0 - std::exp (-dt * 1000.0 * std::pow (1.0 + cf, attackBeta) / attackMs));
            cf = std::expm1 (cd);
        }
        else
            cf = std::max (ctf, cf - dt * releaseA * std::pow (cf, releaseN));
        if (cts > cs)
            cs += (cts - cs) * cA;
        else
            cs = std::max (cts, cs - dt * releaseA2 * std::pow (cs, releaseN2));
        const double g = std::max (gFloor, 1.0 / (1.0 + cf + cs));
        mem += ((1.0 - g) > mem ? mUp : mDn) * ((1.0 - g) - mem);
        const double gp = g + (1.0 - g) * (tiltC0 - tiltC1 * g);
        const double wz = 2.0 * pi * tiltHz * (1.0 + g);
        const double a0 = 1.0 + K * gp / wz, b0 = g * (1.0 + K / wz) / a0, b1 = g * (1.0 - K / wz) / a0, a1 = (1.0 - K * gp / wz) / a0;
        float* ch[2] = { &l[i], &r[i] };
        for (int c = 0; c < 2; ++c)
        {
            const double x = *ch[c];
            xp[c] = x;
            const double y = b0 * x + b1 * fx1[c] - a1 * fy1[c];
            fx1[c] = x;
            fy1[c] = y;
            yc[c] = y;
            double o = y * mk;
            o = o + h2 * o * o + h3 * o * o * o;
            for (int j = 3; j > 0; --j) { tx[c][j] = tx[c][j - 1]; ty[c][j] = ty[c][j - 1]; }
            tx[c][0] = o;
            const double t = tb[0] * tx[c][0] + tb[1] * tx[c][1] + tb[2] * tx[c][2] + tb[3] * tx[c][3] - ta[1] * ty[c][1] - ta[2] * ty[c][2] - ta[3] * ty[c][3];
            ty[c][0] = t;
            *ch[c] = (float) t;
        }
        maxGr = std::max (maxGr, -gainToDb (g));
    }
    last = (float) maxGr;
}
} // namespace opto

// ================================================================== program-dependent peak compressor
// [EXEC] Fitted on the official Renaissance Compressor trial at her state (-15 dB, 2.95:1, 0.5 ms, 160 ms, ARC, Warm,
// Electro) on 1 kHz stairs, tone bursts, pink noise and her takes (dynfit "rc2"): a 64-sample (1.33 ms at 48 kHz)
// delay of the audio against the detector, a linked peak detector (9 ms release), a 21.7 dB-wide soft knee 2.8 dB
// under the threshold, a fast gain path (attack 2.1 x the knob, release 25 ms) and a slow "program" path that
// charges in ~220 ms and releases at a constant rate in linear gain (3.8 /s x 160 ms / the release knob) — so short
// peaks recover in milliseconds and long phrases slowly (its ARC / Electro behaviour at her state). Her takes:
// null -26 dB (stairs and bursts -40 … -50 dB). [model] The knob laws away from her state, ARC / manual, Opto and
// Smooth are not separated yet (REA_rodada123 §3).
namespace progcomp
{
namespace
{
#include "core/RCompStatic.inc"
constexpr double thrOffset = -2.8056, kneeDb = 21.7431, ratioScale = 1.3191, attackScale = 2.1395, fastReleaseMs = 24.5151;
constexpr double slowChargeMs = 222.5718, slowRate = 3.8413, slowShape = -1.1528, detectorReleaseMs = 9.2761;
} // namespace

double measuredStaticDelta (double ratio, double over) noexcept
{
    // bilinear in (ln ratio, level over the threshold); 1 : 1 = no correction, past the last column the last value
    constexpr int nr = (int) std::size (rcStaticRatios), nc = (int) std::size (rcStaticDelta[0]);
    if (ratio <= 1.0 || over <= rcStaticOver0)
        return 0.0;
    const double c = std::min ((over - rcStaticOver0) / rcStaticStep, (double) (nc - 1));
    const int c0 = std::min ((int) c, nc - 2);
    const double cf = c - c0;
    const auto at = [&] (int k) { return rcStaticDelta[k][c0] + (rcStaticDelta[k][c0 + 1] - rcStaticDelta[k][c0]) * cf; };
    const double lr = std::log (ratio);
    if (lr <= std::log (rcStaticRatios[0]))
        return at (0) * lr / std::log (rcStaticRatios[0]);
    for (int k = 0; k + 1 < nr; ++k)
        if (lr <= std::log (rcStaticRatios[k + 1]))
        {
            const double f = (lr - std::log (rcStaticRatios[k])) / (std::log (rcStaticRatios[k + 1]) - std::log (rcStaticRatios[k]));
            return at (k) + (at (k + 1) - at (k)) * f;
        }
    return at (nr - 1);
}

void Compressor::prepare (double sampleRate) noexcept
{
    sr = sampleRate;
    delayLen = std::clamp ((int) std::lround (lookahead * sr / 48000.0), 1, (int) dl.size() - 1);
    reset();
}

void Compressor::reset() noexcept
{
    dl.fill (0.0f);
    dr.fill (0.0f);
    pos = 0;
    env = grf = grs = 0.0;
}

void Compressor::process (float* l, float* r, int n, const Settings& s) noexcept
{
    const double T = s.thresholdDb + thrOffset, R = 1.0 + (std::max (1.0, s.ratio) - 1.0) * (s.smooth && ! s.measuredStatic ? 1.0 : ratioScale), W = kneeDb;
    const double aA = coefMs (std::max (0.01, s.attackMs) * attackScale, sr), sC = coefMs (slowChargeMs, sr);
    const double fR = coefMs (s.manual ? std::max (1.0, s.releaseMs) : fastReleaseMs, sr);
    const double shape = s.opto ? -slowShape : slowShape;
    const double dR = coefMs (detectorReleaseMs, sr), relK = 160.0 / std::max (1.0, s.releaseMs), dt = 1.0 / sr;
    const double makeup = s.gainDb, trim = dbToGain (s.trimDb);
    // round 150: below 1 : 1 the reference expands — the slope per ratio measured at 0.90 / 0.79 / 0.50 : 1 (0.18 / 0.40 /
    // 0.90 dB per dB over the threshold), interpolated on the ratio control's own scale (1.00 at 0.625, 0.5 at 1)
    const bool expand = s.ratio < 0.999;
    double expSlope = 0.0;
    if (expand)
    {
        const double v = 0.625 + 0.375 * std::log (std::max (0.5, s.ratio)) / std::log (0.5);
        static constexpr double at[4] { 0.625, 0.68, 0.75, 1.0 }, sl[4] { 0.0, 0.179, 0.403, 0.902 };
        int k = 0;
        while (k < 2 && v > at[k + 1]) ++k;
        expSlope = sl[k] + (sl[k + 1] - sl[k]) * std::clamp ((v - at[k]) / (at[k + 1] - at[k]), 0.0, 1.0);
    }
    const double Te = s.thresholdDb - 2.28, We = 4.55;
    const int N = (int) dl.size();
    const bool corrected = s.measuredStatic && ! expand && s.ratio > 1.0;
    double maxGr = 0.0, extreme = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double a = std::max (std::abs ((double) l[i]), std::abs ((double) r[i]));
        env = a > env ? a : env + dR * (a - env);
        const double x = gainToDb (env), o = x - T;
        if (expand)
        {
            // round 150 [EXEC] the reference's ratios below 1: an upward expansion above the threshold (2.3 dB lower,
            // a 4.6 dB knee), its slope measured per ratio (REA_rodada150 §6); the boost grows with the attack
            const double oe = x - Te;
            const double b = 2.0 * oe < -We ? 0.0 : (2.0 * std::abs (oe) <= We ? expSlope * (oe + We / 2.0) * (oe + We / 2.0) / (2.0 * We) : expSlope * oe);
            grf += (-b < grf ? aA : fR) * (-b - grf);
            grs = 0.0;
        }
        else
        {
        double y;
        if (2.0 * o < -W) y = x;
        else if (2.0 * std::abs (o) <= W) y = x + (1.0 / R - 1.0) * (o + W / 2.0) * (o + W / 2.0) / (2.0 * W);
        else y = T + o / R;
        // round 152: the measured static curve (a correction over the threshold as the trial measured it)
        const double target = std::max (0.0, x - y - (corrected ? measuredStaticDelta (s.ratio, x - s.thresholdDb) : 0.0));
        grf += (target > grf ? aA : fR) * (target - grf);
        if (s.manual)
            grs = 0.0;
        else if (grf > grs)
            grs += sC * (grf - grs);
        else
        {
            double gs = dbToGain (-grs);
            gs += slowRate * relK * (1.0 + shape * (1.0 - gs)) * dt;
            grs = gs >= 1.0 ? 0.0 : -gainToDb (gs);
            grs = std::max (grs, grf);
        }
        }
        const double gr = expand ? grf : std::max (grf, grs);
        maxGr = std::max (maxGr, gr);
        if (std::abs (gr) > std::abs (extreme)) extreme = gr;
        const double g = (dbToGain (makeup - gr) * s.mix + (1.0 - s.mix)) * trim;
        dl[(std::size_t) pos] = l[i];
        dr[(std::size_t) pos] = r[i];
        int rd = pos - delayLen;
        if (rd < 0) rd += N;
        l[i] = (float) (dl[(std::size_t) rd] * g);
        r[i] = (float) (dr[(std::size_t) rd] * g);
        if (++pos == N) pos = 0;
    }
    last = (float) maxGr;
    change = (float) -extreme;
}
} // namespace progcomp
} // namespace lynx::vmatch
