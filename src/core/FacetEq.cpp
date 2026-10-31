#include "core/FacetEq.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>

namespace lynx::facet
{
namespace
{
constexpr double pi = 3.14159265358979323846;
using cd = std::complex<double>;

// Analog section in normalized frequency (s = jω/ω0): (n2 s² + n1 s + n0) / (d2 s² + d1 s + d0)
struct Analog { double n2 = 0, n1 = 0, n0 = 1, d2 = 0, d1 = 0, d0 = 1; };

cd evalAnalog (const Analog& a, double w) noexcept // w = ω/ω0
{
    const cd s (0.0, w);
    return (a.n2 * s * s + a.n1 * s + a.n0) / (a.d2 * s * s + a.d1 * s + a.d0);
}

double mag2 (const Analog& a, double w) noexcept { return std::norm (evalAnalog (a, w)); }

// Magnitude-matched digital section for an analog section at w0 = 2π f0 / fs. `ok` reports whether the numerator
// was realisable as asked (false = the magnitude at the match point had to be clamped).
Section matched (const Analog& a, double w0, bool& ok) noexcept
{
    Section s;
    ok = true;
    if (a.n1 < 0.0 && a.n2 == a.d2 && a.n0 == a.d0 && a.d2 != 0.0)
    {
        // all pass (round 135): no magnitude to match — the bilinear all-pass at the band frequency, same Q
        const double qb = 1.0 / a.d1, alpha = std::sin (std::min (w0, 0.98 * pi)) / (2.0 * qb), c = std::cos (std::min (w0, 0.98 * pi));
        const double a0 = 1.0 + alpha;
        s.b0 = (1.0 - alpha) / a0; s.b1 = -2.0 * c / a0; s.b2 = 1.0;
        s.a1 = -2.0 * c / a0; s.a2 = (1.0 - alpha) / a0;
        return s;
    }
    const double nyq = pi / w0; // digital Nyquist on the normalized analog axis

    if (a.d2 == 0.0)
    {
        // first order: (n1 s + n0) / (d1 s + d0); pole by impulse invariance, zero matched at DC and Nyquist
        const double wp = std::min (a.d0 / a.d1 * w0, 0.999 * pi);
        const double p = std::exp (-wp);
        const double h0 = std::sqrt (mag2 (a, 0.0)), hn = std::sqrt (mag2 (a, nyq));
        const double sum = h0 * (1.0 - p), diff = hn * (1.0 + p);
        s.b0 = 0.5 * (sum + diff);
        s.b1 = 0.5 * (sum - diff);
        s.a1 = -p;
        return s;
    }

    // poles of d2 s² + d1 s + d0: natural frequency wp, damping zeta
    const double wpn = std::sqrt (a.d0 / a.d2);
    const double zeta = a.d1 / (2.0 * std::sqrt (a.d0 * a.d2));
    const double wp = std::min (wpn * w0, 0.999 * pi);
    const double e = std::exp (-zeta * wp);
    s.a2 = e * e;
    s.a1 = zeta <= 1.0 ? -2.0 * e * std::cos (std::sqrt (1.0 - zeta * zeta) * wp)
                       : -2.0 * e * std::cosh (std::sqrt (zeta * zeta - 1.0) * wp);

    // squared magnitude of a digital polynomial as A0 φ0 + A1 φ1 + A2 φ2 (φ0 = cos²(w/2), φ1 = sin²(w/2), φ2 = 4 φ0 φ1)
    const double A0 = (1.0 + s.a1 + s.a2) * (1.0 + s.a1 + s.a2);
    const double A1 = (1.0 - s.a1 + s.a2) * (1.0 - s.a1 + s.a2);
    const double A2 = -4.0 * s.a2;
    auto den = [&] (double w) {
        const double p1 = std::sin (w * 0.5) * std::sin (w * 0.5), p0 = 1.0 - p1;
        return A0 * p0 + A1 * p1 + A2 * 4.0 * p0 * p1;
    };
    const double wm = std::min (w0, 0.9 * pi);
    const double p1 = std::sin (wm * 0.5) * std::sin (wm * 0.5), p0 = 1.0 - p1;
    const double target = mag2 (a, wm / w0) * den (wm);

    if (a.n1 == 0.0 && a.n0 == 0.0)
    {
        // high-pass: double zero at DC, k (1 − z⁻¹)², matched at the band frequency
        const double k = std::sqrt (target) / (4.0 * p1);
        s.b0 = k; s.b1 = -2.0 * k; s.b2 = k;
        return s;
    }
    if (a.n1 == 0.0 && a.n2 != 0.0 && a.n0 != 0.0)
    {
        // notch: zeros on the unit circle at the notch frequency, gain matched at DC
        const double wz = std::min (std::sqrt (a.n0 / a.n2) * w0, 0.999 * pi);
        const double k = std::sqrt (mag2 (a, 0.0)) * (1.0 + s.a1 + s.a2) / (2.0 - 2.0 * std::cos (wz));
        s.b0 = k; s.b1 = -2.0 * k * std::cos (wz); s.b2 = k;
        return s;
    }

    // numerator matched at DC, Nyquist and the match point (the band frequency, kept inside the band)
    const double B0 = A0 * mag2 (a, 0.0);
    const double B1 = A1 * mag2 (a, nyq);
    double B2 = (target - B0 * p0 - B1 * p1) / (4.0 * p0 * p1);

    const double sb0 = std::sqrt (std::max (0.0, B0)), sb1 = std::sqrt (std::max (0.0, B1));
    const double W = 0.5 * (sb0 + sb1);
    if (B2 < -W * W) { B2 = -W * W; ok = false; }
    s.b0 = 0.5 * (W + std::sqrt (W * W + B2));
    s.b1 = 0.5 * (sb0 - sb1);
    s.b2 = s.b0 != 0.0 ? -B2 / (4.0 * s.b0) : 0.0;
    return s;
}

// matched design; when the numerator cannot be realised (cuts near Nyquist), the reciprocal section is designed
// instead and inverted (its zeros become the poles)
Section matched (const Analog& a, double w0) noexcept
{
    bool ok = true;
    const auto s = matched (a, w0, ok);
    if (ok || a.d2 == 0.0 || a.n2 == 0.0 || a.n0 == 0.0)
        return s;
    const Analog inv { a.d2, a.d1, a.d0, a.n2, a.n1, a.n0 };
    bool ok2 = true;
    const auto r = matched (inv, w0, ok2);
    if (! ok2 || r.b0 == 0.0)
        return s;
    Section out;
    out.b0 = 1.0 / r.b0; out.b1 = r.a1 / r.b0; out.b2 = r.a2 / r.b0;
    out.a1 = r.b1 / r.b0; out.a2 = r.b2 / r.b0;
    return out;
}

double bandQ (const EffectParams& p, int b) noexcept { return q (p[(std::size_t) param (b, 3)]); }

// Butterworth section Qs of an order-n filter, ascending
int butterworthQs (int n, double* qs) noexcept
{
    const int m = n / 2;
    for (int k = 0; k < m; ++k)
        qs[k] = 1.0 / (2.0 * std::sin ((2 * k + 1) * pi / (2.0 * n)));
    std::sort (qs, qs + m);
    return m;
}

// analog sections of one band (normalized to the band frequency); returns the count
int analogSections (const EffectParams& p, int b, Analog* out, double extraDb = 0.0, bool keepFlat = false) noexcept
{
    if (! bandPower (p, b)) return 0; // a band switched off (round 128) keeps its settings and does nothing
    const auto sh = shape (p[(std::size_t) param (b, 0)]);
    const double scale = gainScale (p[(std::size_t) scaleParam]);
    const double g = (gainDb (p[(std::size_t) param (b, 2)]) + extraDb) * scale;
    const double Q = bandQ (p, b);
    const int slope = slopeDb (p[(std::size_t) param (b, 4)]);

    switch (sh)
    {
        case Shape::off: return 0;
        case Shape::bell:
        {
            // symmetric bell: (s² + s·A/Qb + 1) / (s² + s/(A·Qb) + 1), Qb = Q/√2 (measured convention)
            if (std::abs (g) < 1e-9 && ! keepFlat) return 0;
            const double A = std::pow (10.0, g / 40.0), qb = Q / std::sqrt (2.0);
            out[0] = { 1.0, A / qb, 1.0, 1.0, 1.0 / (A * qb), 1.0 };
            return 1;
        }
        case Shape::lowShelf:
        case Shape::highShelf:
        case Shape::tiltShelf:
        {
            if (std::abs (g) < 1e-9 && ! keepFlat) return 0;
            const bool tilt = sh == Shape::tiltShelf;
            const double gg = tilt ? 2.0 * g : g;
            const double A = std::pow (10.0, gg / 40.0), sa = std::sqrt (A);
            const double norm = tilt ? std::pow (10.0, -g / 20.0) : 1.0;
            const bool high = sh != Shape::lowShelf;
            if (slope <= 6)
            {
                // first-order shelf, geometric half-gain point at the band frequency: high (A s + 1)/(s/A + 1), low (s + A)/(s + 1/A)
                if (high) out[0] = { 0.0, A * norm, norm, 0.0, 1.0 / A, 1.0 };
                else out[0] = { 0.0, norm, A * norm, 0.0, 1.0, 1.0 / A };
                return 1;
            }
            // second-order cookbook shelf, Qs = 0.7071·Q^0.4697 (measured), gain independent
            const double qs = std::sqrt (0.5) * std::pow (Q, 0.4697);
            if (high) out[0] = { A * A * norm, A * sa / qs * norm, A * norm, 1.0, sa / qs, A };
            else out[0] = { A * norm, A * sa / qs * norm, A * A * norm, A, sa / qs, 1.0 };
            return 1;
        }
        case Shape::lowCut:
        case Shape::highCut:
        {
            const bool hp = sh == Shape::lowCut;
            const int n = std::max (1, slope / 6);
            double qs[16];
            const int m = butterworthQs (n, qs);
            if (m > 0) qs[m - 1] *= Q / 1.0; // the highest-Q section carries the band's resonance (Q 1 = Butterworth)
            int c = 0;
            for (int k = 0; k < m; ++k)
                out[c++] = hp ? Analog { 1.0, 0.0, 0.0, 1.0, 1.0 / qs[k], 1.0 } : Analog { 0.0, 0.0, 1.0, 1.0, 1.0 / qs[k], 1.0 };
            if (n % 2)
                out[c++] = hp ? Analog { 0.0, 1.0, 0.0, 0.0, 1.0, 1.0 } : Analog { 0.0, 0.0, 1.0, 0.0, 1.0, 1.0 };
            return c;
        }
        case Shape::notch:
        {
            const double qb = Q / std::sqrt (2.0);
            out[0] = { 1.0, 0.0, 1.0, 1.0, 1.0 / qb, 1.0 };
            return 1;
        }
        case Shape::bandPass:
        {
            const double qb = Q / std::sqrt (2.0);
            out[0] = { 0.0, 1.0 / qb, 0.0, 1.0, 1.0 / qb, 1.0 };
            return 1;
        }
        case Shape::allPass: // round 135: (s² − s/Qb + 1) / (s² + s/Qb + 1)
        {
            const double qb = Q / std::sqrt (2.0);
            out[0] = { 1.0, -1.0 / qb, 1.0, 1.0, 1.0 / qb, 1.0 };
            return 1;
        }
        case Shape::flatTilt:
        {
            // round 135: eight first-order shelves 1.25 octaves apart (±5 octaves around the band frequency), each
            // a step of g / 4 dB: a constant slope of g / 5 dB per octave, 0 dB at the band frequency
            if (std::abs (g) < 1e-9 && ! keepFlat) return 0;
            const double A = std::pow (10.0, (g / 4.0) / 40.0);
            double at1 = 1.0;
            for (int k = 0; k < 8; ++k)
            {
                const double c = std::pow (2.0, (k - 3.5) * 1.25);
                out[k] = { 0.0, A / c, 1.0, 0.0, 1.0 / (A * c), 1.0 };
                at1 *= std::sqrt (mag2 (out[k], 1.0));
            }
            out[0].n1 /= at1;
            out[0].n0 /= at1;
            return 8;
        }
    }
    return 0;
}


cd evalDigital (const Section& s, double w) noexcept
{
    const cd z1 = std::polar (1.0, -w), z2 = z1 * z1;
    return (s.b0 + s.b1 * z1 + s.b2 * z2) / (1.0 + s.a1 * z1 + s.a2 * z2);
}

// the FIR grid factor of a phase mode: 1 up to 48 kHz, 2 up to 96 kHz, 4 above
int rateFactor (double sr) noexcept { return sr <= 50000.0 ? 1 : sr <= 100000.0 ? 2 : 4; }
// round 144: the linear-phase block is 1024 (the trial's latencies are 1024 + half the FIR; was 512)
constexpr int naturalBlock = 128, naturalDelay = 192, linearBlock = 1024, firTaps = 8192;

// --- FFT and the partitioned convolver (the FIR phase modes) ------------------------------------------------------

struct Fft
{
    int n = 0;
    std::vector<cd> tw;
    std::vector<int> rev;

    void init (int size)
    {
        n = size;
        tw.resize ((std::size_t) n / 2);
        for (int k = 0; k < n / 2; ++k) tw[(std::size_t) k] = std::polar (1.0, -2.0 * pi * k / n);
        rev.resize ((std::size_t) n);
        int bits = 0;
        while ((1 << bits) < n) ++bits;
        for (int i = 0; i < n; ++i)
        {
            int r = 0;
            for (int b = 0; b < bits; ++b) r |= ((i >> b) & 1) << (bits - 1 - b);
            rev[(std::size_t) i] = r;
        }
    }

    // in place, unscaled (the inverse is n times too large)
    void run (cd* a, bool inverse) const noexcept
    {
        for (int i = 0; i < n; ++i)
            if (i < rev[(std::size_t) i]) std::swap (a[i], a[rev[(std::size_t) i]]);
        for (int len = 2; len <= n; len <<= 1)
        {
            const int half = len / 2, step = n / len;
            for (int i = 0; i < n; i += len)
                for (int j = 0; j < half; ++j)
                {
                    const double wr = tw[(std::size_t) (j * step)].real(), wi = inverse ? -tw[(std::size_t) (j * step)].imag() : tw[(std::size_t) (j * step)].imag();
                    const double xr = a[i + j + half].real(), xi = a[i + j + half].imag();
                    const cd u = a[i + j], v (xr * wr - xi * wi, xr * wi + xi * wr);
                    a[i + j] = u + v;
                    a[i + j + half] = u - v;
                }
        }
    }
};

// Uniformly partitioned overlap-save convolution of a stereo signal (left + j·right) with a real FIR; block P, latency
// P. Buffers are sized once for the largest configuration; a new kernel is cross-faded in over one block.
class Convolver
{
public:
    void allocate (int maxTaps, int maxBlock)
    {
        fdl.assign ((std::size_t) maxTaps * 2, {});
        kern[0].assign ((std::size_t) maxTaps * 2, {});
        kern[1].assign ((std::size_t) maxTaps * 2, {});
        timeBuf.assign ((std::size_t) maxBlock * 2, {});
        acc.assign ((std::size_t) maxBlock * 2, {});
        acc2.assign ((std::size_t) maxBlock * 2, {});
        in.assign ((std::size_t) maxBlock, {});
        out.assign ((std::size_t) maxBlock, {});
    }

    // P must be a power of two whose FFT (2P) is `fft`
    void configure (const Fft& f, int blockSize, int taps) noexcept
    {
        fft = &f;
        P = blockSize;
        K = taps / blockSize;
        reset();
    }

    void reset() noexcept
    {
        std::fill (fdl.begin(), fdl.end(), cd {});
        std::fill (timeBuf.begin(), timeBuf.end(), cd {});
        std::fill (in.begin(), in.end(), cd {});
        std::fill (out.begin(), out.end(), cd {});
        pos = head = 0;
        pending = -1;
    }

    // a new FIR (K·P taps); the first one is used at once, later ones fade in over the next block
    void load (const double* h, bool immediate) noexcept
    {
        const int next = immediate ? active : 1 - active;
        auto* frame = acc2.data();
        for (int k = 0; k < K; ++k)
        {
            for (int i = 0; i < P; ++i) frame[i] = cd (h[k * P + i], 0.0);
            for (int i = P; i < 2 * P; ++i) frame[i] = {};
            fft->run (frame, false);
            std::copy (frame, frame + 2 * P, kern[(std::size_t) next].begin() + (std::ptrdiff_t) k * 2 * P);
        }
        pending = immediate ? -1 : next;
    }

    bool fading() const noexcept { return pending >= 0; }

    void step (float& l, float& r) noexcept
    {
        in[(std::size_t) pos] = cd (l, r);
        const auto y = out[(std::size_t) pos];
        l = (float) y.real();
        r = (float) y.imag();
        if (++pos == P) block();
    }

private:
    void accumulate (int which, cd* dst) noexcept
    {
        const int n2 = 2 * P;
        std::fill (dst, dst + n2, cd {});
        const auto* kr = kern[(std::size_t) which].data();
        for (int k = 0; k < K; ++k)
        {
            const auto* x = fdl.data() + (std::ptrdiff_t) (((head - k + K) % K) * n2);
            const auto* hk = kr + (std::ptrdiff_t) k * n2;
            // plain products (std::complex's operator* checks for infinities, several times slower)
            for (int i = 0; i < n2; ++i)
            {
                const double xr = x[i].real(), xi = x[i].imag(), hr = hk[i].real(), hi = hk[i].imag();
                dst[i] += cd (xr * hr - xi * hi, xr * hi + xi * hr);
            }
        }
        fft->run (dst, true);
    }

    void block() noexcept
    {
        const int n2 = 2 * P;
        auto* tb = timeBuf.data();
        std::copy (tb + P, tb + n2, tb);
        std::copy (in.begin(), in.begin() + P, tb + P);
        auto* x = fdl.data() + (std::ptrdiff_t) head * n2;
        std::copy (tb, tb + n2, x);
        fft->run (x, false);
        const double scale = 1.0 / n2;
        accumulate (active, acc.data());
        if (pending >= 0)
        {
            accumulate (pending, acc2.data());
            for (int i = 0; i < P; ++i)
            {
                const double t = (i + 0.5) / P;
                out[(std::size_t) i] = (acc[(std::size_t) (P + i)] * (1.0 - t) + acc2[(std::size_t) (P + i)] * t) * scale;
            }
            active = pending;
            pending = -1;
        }
        else
            for (int i = 0; i < P; ++i) out[(std::size_t) i] = acc[(std::size_t) (P + i)] * scale;
        head = (head + 1) % K;
        pos = 0;
    }

    const Fft* fft = nullptr;
    int P = 128, K = 1, pos = 0, head = 0, active = 0, pending = -1;
    std::vector<cd> fdl, kern[2], timeBuf, acc, acc2, in, out;
};

// a biquad on doubles (detector / solo filters)
struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1[2] {}, z2[2] {};
    double run (double x, int ch) noexcept
    {
        const double y = b0 * x + z1[ch];
        z1[ch] = b1 * x - a1 * y + z2[ch];
        z2[ch] = b2 * x - a2 * y;
        return y;
    }
    void clear() noexcept { z1[0] = z1[1] = z2[0] = z2[1] = 0.0; }
};

// the band's region: band pass at its frequency and Q (low pass / high pass for the shelves and cuts)
void regionFilter (Biquad& f, const EffectParams& p, int b, double sr) noexcept
{
    const auto sh = shape (p[(std::size_t) param (b, 0)]);
    const double f0 = std::min (hz (p[(std::size_t) param (b, 1)]), 0.45 * sr);
    const double w0 = 2.0 * pi * f0 / sr, cw = std::cos (w0), sw = std::sin (w0);
    const bool low = sh == Shape::lowShelf || sh == Shape::highCut;
    const bool high = sh == Shape::highShelf || sh == Shape::lowCut;
    const double Q = low || high ? std::sqrt (0.5) : std::clamp (q (p[(std::size_t) param (b, 3)]), 0.1, 40.0);
    const double alpha = sw / (2.0 * Q), a0 = 1.0 + alpha;
    if (low) { f.b0 = (1.0 - cw) * 0.5 / a0; f.b1 = (1.0 - cw) / a0; f.b2 = f.b0; }
    else if (high) { f.b0 = (1.0 + cw) * 0.5 / a0; f.b1 = -(1.0 + cw) / a0; f.b2 = f.b0; }
    else { f.b0 = alpha / a0; f.b1 = 0.0; f.b2 = -alpha / a0; }
    f.a1 = -2.0 * cw / a0;
    f.a2 = (1.0 - alpha) / a0;
}


// --- round 152: the spectral dynamics stage (REA_rodada152c) ----------------------------------------------------------
// [EXEC] fitted on the Pro-Q 4 trial (partials and pink noise through a dynamic bell with Spectral on): the bin's weighted
// level W = L + 0.18 · 20 log10 S (S = the band's shape, 0 … 1), the threshold T = max (threshold − 30.5 dB, the loudest W
// − 11.6 dB), the gain 1.066 dB per dB over T, at most the range × S^2.18 (0.33 dB rms over 22 cases); the latency is the
// trial's 5120 samples at 48 kHz. Tilt: the threshold sloped by 4.5 dB/oct around the band frequency [model, direction
// from the pink-noise renders]; density: a blur of the gains across bins [model].
constexpr double specWeightExp = 0.181, specOffsetDb = -30.47, specRelativeDb = 11.58, specSlope = 1.066, specCapExp = 2.18;
constexpr double specTiltDbOct = 4.5;
constexpr double densityBlend = 0.15; // [model] density 100 %: a pure tone cut 0.8 dB less (the trial: −11.2 instead of −12)

// the band's shape at f as a share 0 … 1 of its range (the band with its gain set to the range)
double spectralShare (const EffectParams& p, int b, double f, double refDb) noexcept
{
    if (std::abs (refDb) < 1e-6) return 0.0;
    return std::clamp (analogDb (p, b, f) / refDb, 0.0, 1.0);
}
EffectParams shareParams (const EffectParams& p, int b, double& refDb) noexcept
{
    auto q = p;
    const double range = rangeDb (p[(std::size_t) (rangeParam0 + b)]);
    q[(std::size_t) param (b, 2)] = gainValue (std::clamp (range, -30.0, 30.0));
    q[(std::size_t) scaleParam] = 0.5f; // 100 %
    const double f0 = hz (p[(std::size_t) param (b, 1)]);
    refDb = 0.0;
    for (const double f : { f0, 20.0, 20000.0 }) // a bell's centre, a shelf's far side
    {
        const double v = analogDb (q, b, f);
        if (std::abs (v) > std::abs (refDb)) refDb = v;
    }
    return q;
}
// the law for one band over n bins / partials: W (weighted levels, dB), S (shares) -> gains (dB, signed)
void spectralLaw (const double* W, const double* S, int n, double thresholdDb, double range, double* g) noexcept
{
    double wmax = -400.0;
    for (int i = 0; i < n; ++i)
        if (S[i] > 0.02) wmax = std::max (wmax, W[i]);
    const double T = std::max (thresholdDb + specOffsetDb, wmax - specRelativeDb);
    const double depth = std::abs (range);
    for (int i = 0; i < n; ++i)
    {
        const double over = S[i] > 1e-4 ? std::max (0.0, specSlope * (W[i] - T)) : 0.0;
        const double v = std::min (depth * std::pow (S[i], specCapExp), over);
        g[i] = range < 0.0 ? -v : v;
    }
}

class SpectralStage
{
public:
    void prepare (double sr)
    {
        sampleRate = sr;
        fct = rateFactor (sr);
        N = 4096 * fct;
        H = N / 4;
        R = 2 * N;
        D = N + H;
        bins = N / 2 + 1;
        fft.init (N);
        win.resize ((std::size_t) N);
        for (int i = 0; i < N; ++i) win[(std::size_t) i] = 0.5 - 0.5 * std::cos (2.0 * pi * i / N);
        for (auto* v : { &inL, &inR, &inD }) v->assign ((std::size_t) R, 0.0f);
        accL.assign ((std::size_t) R, 0.0);
        accR.assign ((std::size_t) R, 0.0);
        z.assign ((std::size_t) N, {});
        zd.assign ((std::size_t) N, {});
        level.assign ((std::size_t) bins, -150.0);
        gainDb.assign ((std::size_t) bins, 0.0);
        tmpW.assign ((std::size_t) bins, 0.0);
        tmpG.assign ((std::size_t) bins, 0.0);
        tmpB.assign ((std::size_t) bins, 0.0);
        for (auto& b : bandsData) { b.share.assign ((std::size_t) bins, 0.0); b.weight.assign ((std::size_t) bins, 0.0); }
        reset();
    }

    void reset() noexcept
    {
        for (auto* v : { &inL, &inR, &inD }) std::fill (v->begin(), v->end(), 0.0f);
        std::fill (accL.begin(), accL.end(), 0.0);
        std::fill (accR.begin(), accR.end(), 0.0);
        std::fill (level.begin(), level.end(), -150.0);
        t = 0;
        tDet = 0;
        for (auto& b : bandsData) b.extreme = 0.0;
    }

    int latency() const noexcept { return D; }
    bool active() const noexcept { return anyBand; }

    void configure (const EffectParams& p, double attackMsV, double releaseMsV)
    {
        anyBand = false;
        for (int b = 0; b < bands; ++b)
        {
            auto& d = bandsData[(std::size_t) b];
            d.on = isSpectral (p, b);
            if (! d.on) { d.extreme = 0.0; continue; }
            anyBand = true;
            d.threshold = thresholdDb (p[(std::size_t) (thresholdParam0 + b)]);
            d.range = rangeDb (p[(std::size_t) (rangeParam0 + b)]);
            d.density = density (p[(std::size_t) (densityParam0 + b)]);
            const bool tilt = p[(std::size_t) (spectralTiltParam0 + b)] >= 0.5f;
            const double f0 = hz (p[(std::size_t) param (b, 1)]);
            double ref = 0.0;
            const auto q = shareParams (p, b, ref);
            for (int k = 0; k < bins; ++k)
            {
                const double f = std::max (1.0, (double) k * sampleRate / N);
                const double sh = spectralShare (q, b, f, ref);
                d.share[(std::size_t) k] = sh;
                d.weight[(std::size_t) k] = specWeightExp * 20.0 * std::log10 (std::max (sh, 1e-4)) + (tilt ? specTiltDbOct * std::log2 (f / f0) : 0.0);
            }
        }
        const double frame = (double) H / sampleRate;
        attackA = 1.0 - std::exp (-frame / std::max (1e-4, attackMsV * 0.001));
        releaseA = 1.0 - std::exp (-frame / std::max (1e-4, releaseMsV * 0.001));
    }

    // the detector's samples (before the EQ) for the next n samples of `process`
    void pushDetector (const float* dl, const float* dr, int n) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            double x = 0.0;
            if (dl != nullptr) x = 0.5 * ((double) dl[i] + (double) (dr != nullptr ? dr[i] : dl[i]));
            if (! std::isfinite (x)) x = 0.0;
            inD[(std::size_t) ((tDet + i) % R)] = (float) x;
        }
        tDet += n;
    }

    void process (float* left, float* right, int n) noexcept
    {
        for (int i = 0; i < n; ++i, ++t)
        {
            const auto w = (std::size_t) (t % R);
            inL[w] = std::isfinite (left[i]) ? left[i] : 0.0f;
            inR[w] = std::isfinite (right[i]) ? right[i] : 0.0f;
            const auto rd = (std::size_t) (((t - D) % R + R) % R);
            left[i] = (float) accL[rd];
            right[i] = (float) accR[rd];
            accL[rd] = accR[rd] = 0.0;
            if ((t + 1) % H == 0)
                frame();
        }
    }

    double extreme (int b) const noexcept { return bandsData[(std::size_t) b].extreme; }

private:
    struct Band
    {
        bool on = false;
        double threshold = -20.0, range = 0.0, density = 0.5, extreme = 0.0;
        std::vector<double> share, weight;
    };

    void frame() noexcept
    {
        const long long start = t - N + 1;
        for (int i = 0; i < N; ++i)
        {
            const auto idx = (std::size_t) (((start + i) % R + R) % R);
            z[(std::size_t) i] = cd (inL[idx] * win[(std::size_t) i], inR[idx] * win[(std::size_t) i]);
            zd[(std::size_t) i] = cd (inD[idx] * win[(std::size_t) i], 0.0);
        }
        fft.run (zd.data(), false);
        fft.run (z.data(), false);
        // the detector's level per bin (a sine's peak: |X| · 4 / N for the Hann window), smoothed in time
        for (int k = 0; k < bins; ++k)
        {
            const double l = std::max (-150.0, 20.0 * std::log10 (std::abs (zd[(std::size_t) k]) * 4.0 / N + 1e-12));
            auto& s = level[(std::size_t) k];
            s += (l > s ? attackA : releaseA) * (l - s);
        }
        std::fill (gainDb.begin(), gainDb.end(), 0.0);
        for (auto& d : bandsData)
        {
            if (! d.on) continue;
            // a partial's whole window lobe (±2 bins) takes its peak's level, so the lobe moves as one
            for (int k = 0; k < bins; ++k)
            {
                double lv = level[(std::size_t) k];
                for (int j = std::max (0, k - 2 * fct); j <= std::min (bins - 1, k + 2 * fct); ++j) lv = std::max (lv, level[(std::size_t) j]);
                tmpW[(std::size_t) k] = lv + d.weight[(std::size_t) k];
            }
            spectralLaw (tmpW.data(), d.share.data(), bins, d.threshold, d.range, tmpG.data());
            // density: the gains blurred across bins (a triangle of 1 … 5 bins each side), blended in as density^4
            const double a = densityBlend * std::pow (d.density, 3.0);
            if (a > 1e-4)
            {
                const int hw = (2 + (int) std::lround (6.0 * d.density)) * fct;
                for (int k = 0; k < bins; ++k)
                {
                    double acc = 0.0, wsum = 0.0;
                    for (int j = -hw; j <= hw; ++j)
                    {
                        const int kk = std::clamp (k + j, 0, bins - 1);
                        const double wt = (double) (hw + 1 - std::abs (j));
                        acc += wt * tmpG[(std::size_t) kk];
                        wsum += wt;
                    }
                    tmpB[(std::size_t) k] = tmpG[(std::size_t) k] + a * (acc / wsum - tmpG[(std::size_t) k]);
                }
                std::copy (tmpB.begin(), tmpB.end(), tmpG.begin());
            }
            double ex = 0.0;
            for (int k = 0; k < bins; ++k)
            {
                gainDb[(std::size_t) k] += tmpG[(std::size_t) k];
                if (std::abs (tmpG[(std::size_t) k]) > std::abs (ex)) ex = tmpG[(std::size_t) k];
            }
            d.extreme = ex;
        }
        // one real gain per bin on both halves of the spectrum (left + j·right stays separable)
        for (int k = 0; k < bins; ++k)
        {
            const double g = std::pow (10.0, gainDb[(std::size_t) k] / 20.0);
            z[(std::size_t) k] *= g;
            if (k > 0 && k < N / 2) z[(std::size_t) (N - k)] *= g;
        }
        fft.run (z.data(), true);
        const double scale = 1.0 / (N * 1.5); // the inverse's n, Hann² at 75 % overlap sums to 1.5
        for (int i = 0; i < N; ++i)
        {
            const auto idx = (std::size_t) (((start + i) % R + R) % R);
            const double w = win[(std::size_t) i] * scale;
            accL[idx] += z[(std::size_t) i].real() * w;
            accR[idx] += z[(std::size_t) i].imag() * w;
        }
    }

    double sampleRate = 48000.0, attackA = 1.0, releaseA = 0.1;
    int fct = 1, N = 4096, H = 1024, R = 8192, D = 5120, bins = 2049;
    long long t = 0, tDet = 0;
    bool anyBand = false;
    Fft fft;
    std::vector<double> win, accL, accR, level, gainDb, tmpW, tmpG, tmpB;
    std::vector<float> inL, inR, inD;
    std::vector<cd> z, zd;
    std::array<Band, bands> bandsData {};
};

class FacetEqEffect final : public Effect
{
public:
    explicit FacetEqEffect (const EffectParams* initial)
    {
        if (initial != nullptr)
        {
            mode = phase ((*initial)[(std::size_t) phaseParam]);
            allocRes = resolution ((*initial)[(std::size_t) resolutionParam]);
            initialParams = *initial;
            haveInitial = true;
        }
    }

    void prepare (double sr) noexcept override
    {
        sampleRate = sr;
        const int fct = rateFactor (sr);
        // round 144: the buffers are sized for this instance's linear-phase resolution (a new resolution chosen in
        // the editor builds a new instance: RackRenderer's reuse rule; automation of it waits for the next prepare)
        if (seenParams) allocRes = resolution (current[(std::size_t) resolutionParam]);
        natTaps = firTaps * fct;
        linTaps = 2 * linearHalfTaps (allocRes, sr);
        const int most = std::max (natTaps, linTaps);
        fftNatural.init (2 * naturalBlock * fct);
        fftLinear.init (2 * linearBlock * fct);
        fftDesignNat.init (natTaps);
        fftDesignLin.init (linTaps);
        conv.allocate (most, linearBlock * fct);
        spec.assign ((std::size_t) most, {});
        fir.assign ((std::size_t) most, 0.0);
        delayLen = std::max (latencyFor (Phase::linear, sr, allocRes), latencyFor (Phase::natural, sr)) + 1;
        delay[0].assign ((std::size_t) delayLen, 0.0f);
        delay[1].assign ((std::size_t) delayLen, 0.0f);
        cached.fill (-1.0f);
        spectral.prepare (sr); // round 152 (with the slot's parameters, so PDC knows its latency from the start)
        {
            // round 166f: the comparative EQ's delay lines: the longest latency this instance can have + one block
            std::uint32_t len = 1;
            while (len < (std::uint32_t) (latencyFor (Phase::linear, sr, allocRes) + spectralLatency (sr) + compareMaxBlock + 1)) len <<= 1;
            for (auto* r : { &srcRing[0], &srcRing[1], &tgtRing[0], &tgtRing[1] }) r->assign (len, 0.0f);
            cmpMask = len - 1;
            cmpStep = (float) (1.0 / (0.01 * sr));
            powCoef = 1.0 - std::exp (-1.0 / (3.0 * sr));
        }
        if (seenParams || haveInitial)
        {
            const auto& q = seenParams ? current : initialParams;
            spectral.configure (q, attackMs (q[(std::size_t) attackParam]), releaseMs (q[(std::size_t) releaseParam]));
        }
        configureMode (mode);
        reset();
    }

    void reset() noexcept override
    {
        for (auto& band : state)
            for (auto& st : band)
                st = {};
        for (auto& d : det) { d.filter.clear(); d.ms = 0.0; d.gain = 0.0; }
        solo.clear();
        conv.reset();
        firLoaded = false;
        for (auto& d : delay) std::fill (d.begin(), d.end(), 0.0f);
        delayPos = 0;
        dynUsed.fill (0.0);
        spectral.reset();
        for (auto& m : meters) m.store (0.0f, std::memory_order_relaxed);
        for (auto* r : { &srcRing[0], &srcRing[1], &tgtRing[0], &tgtRing[1] }) std::fill (r->begin(), r->end(), 0.0f);
        cmpPos = 0;
        powSrc = powTgt = 0.0;
        tgtGain = 1.0f;
    }

    void setSidechain (const float* left, const float* right) noexcept override
    {
        sideL = left;
        sideR = right;
        sideGiven = true;
    }

    float controlOutput (int o) const noexcept override
    {
        return o >= 0 && o < bands ? meters[(std::size_t) o].load (std::memory_order_relaxed) / 60.0f : 0.0f;
    }

    int latencySamples() const noexcept override { return latencyNow.load (std::memory_order_relaxed); }

    void process (float* left, float* right, int n, const EffectParams& params, double) noexcept override
    {
        bool changed = false;
        for (int i = 0; i < paramCount; ++i)
            if (params[(std::size_t) i] != cached[(std::size_t) i]) { changed = true; break; }
        if (changed)
            update (params);

        // round 166f: comparative EQ — keep the source (this input) and the target (the sidechain) for `listen`
        const auto want = n <= compareMaxBlock && cmpMask > 0 ? listen (params[(std::size_t) listenParam]) : Listen::eq;
        const bool comparing = want != Listen::eq || gEq < 1.0f;
        const bool keep = comparing || (sideGiven && cmpMask > 0 && n <= compareMaxBlock) || (latencyNow.load (std::memory_order_relaxed) > 0 && cmpMask > 0 && n <= compareMaxBlock);
        const auto blockPos = cmpPos;
        if (keep)
        {
            const float* tl = sideGiven ? sideL : nullptr;
            const float* tr = sideGiven ? (sideR != nullptr ? sideR : sideL) : nullptr;
            double ps = powSrc, pt = powTgt;
            const double a = powCoef;
            for (int i = 0; i < n; ++i)
            {
                const auto k = (std::size_t) ((blockPos + (std::uint32_t) i) & cmpMask);
                const float sl = left[i], sr = right[i];
                const float xl = tl != nullptr ? tl[i] : 0.0f, xr = tr != nullptr ? tr[i] : 0.0f;
                srcRing[0][k] = sl; srcRing[1][k] = sr;
                tgtRing[0][k] = xl; tgtRing[1][k] = xr;
                ps += a * (0.5 * ((double) sl * sl + (double) sr * sr) - ps);
                pt += a * (0.5 * ((double) xl * xl + (double) xr * xr) - pt);
            }
            powSrc = std::isfinite (ps) && ps > 1e-30 ? ps : 0.0;
            powTgt = std::isfinite (pt) && pt > 1e-30 ? pt : 0.0;
            cmpPos = blockPos + (std::uint32_t) n;
        }

        // the detector: the slot's sidechain when asked for and routed (nullptr = a silent source), else the input
        const bool useSide = sideGiven && sidechainDetector (params[(std::size_t) detectorParam]);
        const float* dl = useSide ? sideL : left;
        const float* dr = useSide ? (sideR != nullptr ? sideR : sideL) : right;
        sideGiven = false;
        sideL = sideR = nullptr;
        // round 152: the spectral stage hears the detector before the EQ changes the buffers
        const bool spectralNow = spectral.active();
        if (spectralNow)
            spectral.pushDetector (dl, dr, n);

        constexpr int chunk = 32;
        for (int off = 0; off < n; off += chunk)
        {
            const int m = std::min (chunk, n - off);
            if (anyDynamic)
                detect (dl != nullptr ? dl + off : nullptr, dr != nullptr ? dr + off : nullptr, left + off, right + off, m, useSide);

            if (soloOn)
            {
                runSolo (left + off, right + off, m);
                continue;
            }
            if (mode == Phase::zeroLatency)
            {
                if (anyDynamic) redesignDynamic (false);
                runIir (left + off, right + off, m, false);
            }
            else
            {
                sinceDesign += m;
                if (! firLoaded || (sinceDesign >= updateEvery && ! conv.fading() && (firDirty || (anyDynamic && dynamicMoved()))))
                    loadFir();
                for (int i = 0; i < m; ++i) conv.step (left[off + i], right[off + i]);
                // bands on one channel or on mid / side (round 128) are not in the stereo FIR: they run as the
                // zero-latency sections after it (minimum phase)
                if (anyPlaced)
                {
                    if (anyDynamic) redesignDynamic (true);
                    runIir (left + off, right + off, m, true);
                }
            }
        }

        if (spectralNow)
            spectral.process (left, right, n);

        if (outMidSide)
            for (int i = 0; i < n; ++i)
            {
                const float mid = 0.5f * (left[i] + right[i]) * outL, side = 0.5f * (left[i] - right[i]) * outR;
                left[i] = mid + side;
                right[i] = mid - side;
            }
        else if (outL != outR)
            for (int i = 0; i < n; ++i) { left[i] *= outL; right[i] *= outR; }
        else if (outL != 1.0f)
            for (int i = 0; i < n; ++i) { left[i] *= outL; right[i] *= outL; }
        if (characterNow > 0) // round 135: the character stage (Clean = untouched)
            for (int i = 0; i < n; ++i)
            {
                left[i] = characterSample (characterNow, left[i]);
                right[i] = characterSample (characterNow, right[i]);
            }
        if (comparing && keep) // round 166f: the source / the target instead of (crossfaded with) the EQ's output
        {
            const auto wantGain = params[(std::size_t) targetLevelParam] >= 0.5f ? (float) targetMatchGain (powSrc, powTgt) : 1.0f;
            const float g0 = tgtGain, g1 = tgtGain + 0.25f * (wantGain - tgtGain); // the level match moves gently per block
            tgtGain = g1;
            const float tE = want == Listen::eq ? 1.0f : 0.0f, tS = want == Listen::source ? 1.0f : 0.0f, tT = want == Listen::target ? 1.0f : 0.0f;
            const auto lat = (std::uint32_t) std::max (0, latencyNow.load (std::memory_order_relaxed));
            const float step = cmpStep;
            auto toward = [step] (float g, float t) { return g < t ? std::min (t, g + step) : std::max (t, g - step); };
            for (int i = 0; i < n; ++i)
            {
                gEq = toward (gEq, tE); gSrc = toward (gSrc, tS); gTgt = toward (gTgt, tT);
                const auto k = (std::size_t) ((blockPos + (std::uint32_t) i - lat) & cmpMask);
                const float tg = (g0 + (g1 - g0) * ((float) i / (float) n)) * gTgt;
                left[i] = gEq * left[i] + gSrc * srcRing[0][k] + tg * tgtRing[0][k];
                right[i] = gEq * right[i] + gSrc * srcRing[1][k] + tg * tgtRing[1][k];
            }
        }
        for (int b = 0; b < bands; ++b)
            meters[(std::size_t) b].store ((float) (spectralNow && isSpectral (current, b) ? spectral.extreme (b) : det[(std::size_t) b].gain), std::memory_order_relaxed);
    }

private:
    struct State { double z1[2] {}, z2[2] {}; };
    struct Detector
    {
        Biquad filter;
        double ms = 0.0, gain = 0.0, msCoef = 0.0, threshold = -20.0, range = 0.0;
        bool on = false;
    };

    void configureMode (Phase p) noexcept
    {
        mode = p;
        const int fct = rateFactor (sampleRate);
        taps = p == Phase::linear ? linTaps : natTaps;
        if (p == Phase::natural) conv.configure (fftNatural, naturalBlock * fct, taps);
        else if (p == Phase::linear) conv.configure (fftLinear, linearBlock * fct, taps);
        // a long FIR is redesigned less often (its design is an FFT of its length)
        updateEvery = std::max ({ 1024 * fct, p == Phase::linear ? linearBlock * fct : naturalBlock * fct, taps / 16 });
        firLoaded = false;
        storeLatency();
    }

    // round 152: the phase mode's latency plus the spectral stage's when a band is spectral
    void storeLatency() noexcept
    {
        latencyNow.store (latencyFor (mode, sampleRate, allocRes) + (spectral.active() ? spectral.latency() : 0), std::memory_order_relaxed);
    }

    void update (const EffectParams& params) noexcept
    {
        for (int i = 0; i < paramCount; ++i) cached[(std::size_t) i] = params[(std::size_t) i];
        current = params;
        seenParams = true;
        const auto newMode = phase (params[(std::size_t) phaseParam]);
        if (newMode != mode)
        {
            configureMode (newMode);
            conv.reset();
            for (auto& d : delay) std::fill (d.begin(), d.end(), 0.0f);
        }
        anyDynamic = false;
        for (int b = 0; b < bands; ++b)
        {
            auto& d = det[(std::size_t) b];
            const bool was = d.on;
            d.on = isDynamic (params, b) && ! isSpectral (params, b); // round 152: a spectral band moves in the STFT stage
            d.threshold = thresholdDb (params[(std::size_t) (thresholdParam0 + b)]);
            d.range = rangeDb (params[(std::size_t) (rangeParam0 + b)]);
            if (d.on)
            {
                regionFilter (d.filter, params, b, sampleRate);
                const double f0 = hz (params[(std::size_t) param (b, 1)]);
                d.msCoef = 1.0 - std::exp (-1.0 / (std::max (0.001, 1.5 / f0) * sampleRate));
                anyDynamic = true;
                if (! was) { d.filter.clear(); d.ms = 0.0; }
            }
            else
                d.gain = 0.0;

            const int before = counts[(std::size_t) b];
            counts[(std::size_t) b] = facet::design (params, b, sampleRate, sections[(std::size_t) b], d.gain);
            if (d.on && counts[(std::size_t) b] == 0) // a dynamic band at 0 dB keeps its (flat) section to move it later
                counts[(std::size_t) b] = designKeep (b, d.gain);
            if (counts[(std::size_t) b] != before) // the structure changed: start this band clean
                for (auto& st : state[(std::size_t) b]) st = {};
            dynUsed[(std::size_t) b] = d.gain;
        }
        {
            const bool wasSpectral = spectral.active();
            spectral.configure (params, attackMs (params[(std::size_t) attackParam]), releaseMs (params[(std::size_t) releaseParam]));
            if (spectral.active() != wasSpectral)
            {
                spectral.reset();
                storeLatency();
            }
        }
        attackCoef = 1.0 - std::exp (-1.0 / (attackMs (params[(std::size_t) attackParam]) * 0.001 * sampleRate));
        releaseCoef = 1.0 - std::exp (-1.0 / (releaseMs (params[(std::size_t) releaseParam]) * 0.001 * sampleRate));
        outGain = (float) std::pow (10.0, outputDb (params[(std::size_t) outputParam]) / 20.0);
        anyPlaced = anyMidSide = false;
        for (int b = 0; b < bands; ++b)
        {
            placed[(std::size_t) b] = placement (params[(std::size_t) (placementParam0 + b)]);
            if (placed[(std::size_t) b] != Placement::stereo && counts[(std::size_t) b] > 0) anyPlaced = true;
            if ((placed[(std::size_t) b] == Placement::mid || placed[(std::size_t) b] == Placement::side) && counts[(std::size_t) b] > 0) anyMidSide = true;
        }
        // the output stage: level, auto gain, polarity, pan (left / right or mid / side)
        float level = outGain;
        if (params[(std::size_t) autoGainParam] >= 0.5f) level *= (float) std::pow (10.0, -autoGainDb (params, sampleRate) / 20.0);
        if (params[(std::size_t) invertParam] >= 0.5f) level = -level;
        const auto pan = panAmount (params[(std::size_t) panParam]);
        outL = level * (float) (pan > 0.0 ? 1.0 - pan : 1.0);
        outR = level * (float) (pan < 0.0 ? 1.0 + pan : 1.0);
        outMidSide = panMidSide (params[(std::size_t) panModeParam]) && outL != outR;
        characterNow = character (params[(std::size_t) characterParam]);

        const int sb = soloBand (params[(std::size_t) soloParam]);
        const bool wasSolo = soloOn;
        soloOn = sb >= 0 && shape (params[(std::size_t) param (sb, 0)]) != Shape::off;
        if (soloOn) regionFilter (solo, params, sb, sampleRate);
        if (soloOn && ! wasSolo) solo.clear();
        firDirty = true;
    }

    int designKeep (int b, double extra) noexcept
    {
        Analog a[maxSections];
        const int n = analogSections (current, b, a, extra, true);
        const double f0 = std::min (hz (current[(std::size_t) param (b, 1)]), 0.4999 * sampleRate);
        const double w0 = 2.0 * pi * f0 / sampleRate;
        for (int k = 0; k < n; ++k) sections[(std::size_t) b][(std::size_t) k] = matched (a[k], w0);
        return n;
    }

    // detector levels and the dynamic gains, sample by sample over one chunk
    void detect (const float* dl, const float* dr, const float* inL, const float* inR, int m, bool side) noexcept
    {
        for (int b = 0; b < bands; ++b)
        {
            auto& d = det[(std::size_t) b];
            if (! d.on) continue;
            for (int i = 0; i < m; ++i)
            {
                double x = 0.0;
                if (side) x = dl != nullptr ? 0.5 * ((double) dl[i] + (double) dr[i]) : 0.0;
                else x = 0.5 * ((double) inL[i] + (double) inR[i]);
                if (! std::isfinite (x)) x = 0.0;
                const double y = d.filter.run (x, 0);
                d.ms += d.msCoef * (y * y - d.ms);
                const double level = 10.0 * std::log10 (2.0 * d.ms + 1e-20);
                const double target = dynamicGainDb (level, d.threshold, d.range);
                d.gain += (std::abs (target) > std::abs (d.gain) ? attackCoef : releaseCoef) * (target - d.gain);
            }
            if (std::abs (d.ms) < 1e-30) d.ms = 0.0;
        }
    }

    // `placedOnly`: in the FIR modes only the bands outside the FIR (one channel, mid or side) run as sections
    void redesignDynamic (bool placedOnly) noexcept
    {
        for (int b = 0; b < bands; ++b)
        {
            const auto& d = det[(std::size_t) b];
            if (placedOnly && placed[(std::size_t) b] == Placement::stereo) continue;
            if (! d.on || std::abs (d.gain - dynUsed[(std::size_t) b]) < 0.01) continue;
            const int before = counts[(std::size_t) b];
            const int c = designKeep (b, d.gain);
            if (c != before)
            {
                for (auto& st : state[(std::size_t) b]) st = {};
                counts[(std::size_t) b] = c;
            }
            dynUsed[(std::size_t) b] = d.gain;
        }
    }

    bool dynamicMoved() const noexcept
    {
        for (int b = 0; b < bands; ++b)
            if (det[(std::size_t) b].on && std::abs (det[(std::size_t) b].gain - dynUsed[(std::size_t) b]) > 0.05) return true;
        return false;
    }

    // the bands as digital sections: stereo bands on both channels, left / right bands on theirs, then mid / side
    // bands on the mid / side signals (round 128). `placedOnly`: only the bands outside the FIR (the FIR modes).
    void runIir (float* left, float* right, int n, bool placedOnly) noexcept
    {
        for (int b = 0; b < bands; ++b)
        {
            const auto pl = placed[(std::size_t) b];
            if (pl == Placement::mid || pl == Placement::side || (placedOnly && pl == Placement::stereo)) continue;
            runBand (b, left, right, n, pl == Placement::left ? 1 : pl == Placement::right ? 2 : 3);
        }
        if (! anyMidSide) return;
        for (int i = 0; i < n; ++i)
        {
            const float l = left[i], r = right[i];
            left[i] = 0.5f * (l + r);
            right[i] = 0.5f * (l - r);
        }
        for (int b = 0; b < bands; ++b)
        {
            const auto pl = placed[(std::size_t) b];
            if (pl == Placement::mid) runBand (b, left, right, n, 1);
            else if (pl == Placement::side) runBand (b, left, right, n, 2);
        }
        for (int i = 0; i < n; ++i)
        {
            const float m = left[i], sd = right[i];
            left[i] = m + sd;
            right[i] = m - sd;
        }
    }

    // one band's sections on the channels in `mask` (bit 0 = the first buffer, bit 1 = the second)
    void runBand (int b, float* left, float* right, int n, int mask) noexcept
    {
        for (int k = 0; k < counts[(std::size_t) b]; ++k)
        {
            const auto& c = sections[(std::size_t) b][(std::size_t) k];
            auto& st = state[(std::size_t) b][(std::size_t) k];
            for (int ch = 0; ch < 2; ++ch)
            {
                if ((mask & (1 << ch)) == 0) continue;
                float* x = ch == 0 ? left : right;
                double z1 = st.z1[ch], z2 = st.z2[ch];
                for (int i = 0; i < n; ++i)
                {
                    const double in = x[i];
                    const double y = c.b0 * in + z1;
                    z1 = c.b1 * in - c.a1 * y + z2;
                    z2 = c.b2 * in - c.a2 * y;
                    x[i] = (float) y;
                }
                // denormal guard
                st.z1[ch] = std::abs (z1) < 1e-30 ? 0.0 : z1;
                st.z2[ch] = std::abs (z2) < 1e-30 ? 0.0 : z2;
            }
        }
    }

    // solo: the band's region alone (delayed like the FIR modes so switching does not jump)
    void runSolo (float* left, float* right, int n) noexcept
    {
        const int lat = mode == Phase::zeroLatency ? 0 : latencyFor (mode, sampleRate, allocRes);
        for (int i = 0; i < n; ++i)
        {
            const float yl = (float) solo.run (left[i], 0), yr = (float) solo.run (right[i], 1);
            if (lat == 0) { left[i] = yl; right[i] = yr; continue; }
            delay[0][(std::size_t) delayPos] = yl;
            delay[1][(std::size_t) delayPos] = yr;
            const int rd = (delayPos - lat + delayLen) % delayLen;
            left[i] = delay[0][(std::size_t) rd];
            right[i] = delay[1][(std::size_t) rd];
            delayPos = (delayPos + 1) % delayLen;
        }
    }

    // the FIR of the current response (analog magnitude; analog phase in natural mode), windowed around its delay
    void loadFir() noexcept
    {
        const int M = taps;
        const bool natural = mode == Phase::natural;
        const int fct = rateFactor (sampleRate);
        const int D = natural ? naturalDelay * fct : M / 2;
        // every band's analog sections once, then the product of their responses on the FFT grid
        Analog a[bands * maxSections];
        double f0[bands * maxSections];
        int used = 0;
        for (int b = 0; b < bands; ++b)
        {
            if (shape (current[(std::size_t) param (b, 0)]) == Shape::off || placed[(std::size_t) b] != Placement::stereo) continue;
            const int c = analogSections (current, b, a + used, det[(std::size_t) b].on ? det[(std::size_t) b].gain : 0.0);
            for (int k = 0; k < c; ++k) f0[used + k] = hz (current[(std::size_t) param (b, 1)]);
            used += c;
        }
        const auto response = [&] (double f, double& hr, double& hi) noexcept {
            hr = 1.0;
            hi = 0.0;
            for (int j = 0; j < used; ++j)
            {
                const auto& s = a[j];
                const double w = f / f0[j], w2 = w * w;
                const double nr = s.n0 - s.n2 * w2, ni = s.n1 * w, dr = s.d0 - s.d2 * w2, di = s.d1 * w;
                const double dd = dr * dr + di * di;
                const double qr = (nr * dr + ni * di) / dd, qi = (ni * dr - nr * di) / dd;
                const double tr = hr * qr - hi * qi;
                hi = hr * qi + hi * qr;
                hr = tr;
            }
        };
        // round 144: an FIR longer than the old 8192 taps (High … Maximum) reads the magnitude from a log-spaced grid
        // (designGrid points from the first bin to Nyquist, interpolated in log frequency) instead of every bin
        const bool gridded = ! natural && M > firTaps * fct;
        const double gLo = std::log (sampleRate / M), gHi = std::log (0.5 * sampleRate);
        if (gridded)
            for (int g = 0; g < designGrid; ++g)
            {
                double hr, hi;
                response (std::exp (gLo + (gHi - gLo) * g / (designGrid - 1)), hr, hi);
                grid[(std::size_t) g] = std::sqrt (hr * hr + hi * hi);
            }
        const cd rot = std::polar (1.0, -2.0 * pi * D / M);
        cd phasor (1.0, 0.0);
        for (int k = 0; k <= M / 2; ++k)
        {
            const double f = std::max ((double) k * sampleRate / M, 0.01);
            cd h;
            if (gridded)
            {
                const double x = k == 0 ? 0.0 : (std::log (f) - gLo) / (gHi - gLo) * (designGrid - 1);
                const int g = std::clamp ((int) x, 0, designGrid - 2);
                const double t = std::clamp (x - g, 0.0, 1.0);
                h = cd (grid[(std::size_t) g] * (1.0 - t) + grid[(std::size_t) (g + 1)] * t, 0.0);
                if (k == 0) { double hr, hi; response (0.01, hr, hi); h = cd (std::sqrt (hr * hr + hi * hi), 0.0); }
            }
            else
            {
                double hr, hi;
                response (f, hr, hi);
                h = natural ? cd (hr, hi) : cd (std::sqrt (hr * hr + hi * hi), 0.0);
            }
            h = cd (h.real() * phasor.real() - h.imag() * phasor.imag(), h.real() * phasor.imag() + h.imag() * phasor.real());
            if ((k & 255) == 255) phasor = std::polar (1.0, -2.0 * pi * (double) (k + 1) * D / M); // no drift
            else phasor *= rot;
            if (k == M / 2 || k == 0) h = cd (h.real(), 0.0);
            spec[(std::size_t) k] = h;
            if (k > 0 && k < M / 2) spec[(std::size_t) (M - k)] = std::conj (h);
        }
        (natural ? fftDesignNat : fftDesignLin).run (spec.data(), true);
        for (int i = 0; i < M; ++i)
        {
            const double w = i < D ? 0.5 * (1.0 - std::cos (pi * (i + 0.5) / D)) : 0.5 * (1.0 + std::cos (pi * (i - D) / (double) (M - D)));
            fir[(std::size_t) i] = spec[(std::size_t) i].real() / M * w;
        }
        conv.load (fir.data(), ! firLoaded);
        firLoaded = true;
        firDirty = false;
        sinceDesign = 0;
        for (int b = 0; b < bands; ++b) dynUsed[(std::size_t) b] = det[(std::size_t) b].gain;
    }

    double sampleRate = 48000.0;
    Phase mode = Phase::zeroLatency;
    EffectParams current {};
    std::array<float, paramCount> cached {};
    std::array<std::array<Section, maxSections>, bands> sections {};
    std::array<std::array<State, maxSections>, bands> state {};
    std::array<int, bands> counts {};
    std::array<Detector, bands> det {};
    std::array<double, bands> dynUsed {};
    std::array<std::atomic<float>, bands> meters {};
    double attackCoef = 0.1, releaseCoef = 0.001;
    float outGain = 1.0f, outL = 1.0f, outR = 1.0f;
    int characterNow = 0;
    std::array<Placement, bands> placed {};
    bool anyPlaced = false, anyMidSide = false, outMidSide = false;
    bool anyDynamic = false, soloOn = false, firLoaded = false, firDirty = true, sideGiven = false;
    Biquad solo;
    const float* sideL = nullptr;
    const float* sideR = nullptr;
    Fft fftNatural, fftLinear, fftDesignNat, fftDesignLin;
    Convolver conv;
    int taps = firTaps, natTaps = firTaps, linTaps = firTaps, allocRes = 1, updateEvery = 1024, sinceDesign = 0;
    bool seenParams = false;
    static constexpr int designGrid = 4096;
    std::array<double, designGrid> grid {};
    std::vector<cd> spec;
    std::vector<double> fir;
    std::vector<float> delay[2];
    int delayLen = 1, delayPos = 0;
    std::atomic<int> latencyNow { 0 };
    SpectralStage spectral;
    EffectParams initialParams {};
    bool haveInitial = false;
    // round 166f: comparative EQ — the source (the input) and the target (the sidechain), delayed by the latency
    static constexpr int compareMaxBlock = 16384;
    std::vector<float> srcRing[2], tgtRing[2];
    std::uint32_t cmpMask = 0, cmpPos = 0;
    float gEq = 1.0f, gSrc = 0.0f, gTgt = 0.0f, cmpStep = 1.0f / 480.0f, tgtGain = 1.0f;
    double powSrc = 0.0, powTgt = 0.0, powCoef = 1e-5;
};
} // namespace


// the nine original shapes sit at k / 8 (saved projects); round 135's at 1/16 (flat tilt) and 3/16 (all pass)
Shape shape (float v) noexcept
{
    const int k = std::clamp ((int) std::lround (v * 16.0f), 0, 16);
    if (k % 2 == 0) return (Shape) (k / 2);
    if (k == 1 && std::abs (v - 1.0f / 16.0f) < 0.02f) return Shape::flatTilt;
    if (k == 3 && std::abs (v - 3.0f / 16.0f) < 0.02f) return Shape::allPass;
    return (Shape) std::clamp ((int) std::lround (v * 8.0f), 0, 8);
}
float shapeValue (Shape s) noexcept
{
    if (s == Shape::flatTilt) return 1.0f / 16.0f;
    if (s == Shape::allPass) return 3.0f / 16.0f;
    return (float) (int) s / 8.0f;
}
const std::array<Shape, shapeCount - 1>& menuShapes() noexcept
{
    static const std::array<Shape, shapeCount - 1> m { Shape::bell, Shape::lowShelf, Shape::lowCut, Shape::highShelf, Shape::highCut,
                                                       Shape::notch, Shape::bandPass, Shape::tiltShelf, Shape::flatTilt, Shape::allPass };
    return m;
}
int character (float v) noexcept { return std::clamp ((int) std::lround (v * 2.0f), 0, 2); }
float characterSample (int c, float x) noexcept
{
    // Subtle: a soft knee that bends hot peaks a little (odd harmonics); Warm: an earlier, rounder knee. Both are unity
    // at low level (below about −20 dBFS the curve is within 0.1 dB of a straight line).
    if (c == 1) return x / std::sqrt (1.0f + 0.12f * x * x);
    if (c == 2) return x / std::sqrt (1.0f + 0.3f * x * x) * (1.0f + 0.02f * std::min (1.0f, x * x));
    return x;
}
double hz (float v) noexcept { return 10.0 * std::pow (3000.0, (double) std::clamp (v, 0.0f, 1.0f)); }
float hzValue (double f) noexcept { return (float) std::clamp (std::log (f / 10.0) / std::log (3000.0), 0.0, 1.0); }
double gainDb (float v) noexcept { return ((double) v - 0.5) * 60.0; }
float gainValue (double db) noexcept { return (float) std::clamp (db / 60.0 + 0.5, 0.0, 1.0); }
double q (float v) noexcept { return 0.025 * std::pow (1600.0, (double) std::clamp (v, 0.0f, 1.0f)); }
float qValue (double qq) noexcept { return (float) std::clamp (std::log (qq / 0.025) / std::log (1600.0), 0.0, 1.0); }
int slopeDb (float v) noexcept { return slopes[(std::size_t) std::clamp ((int) std::lround (v * 8.0f), 0, 8)]; }
float slopeValue (int d) noexcept
{
    for (int i = 0; i < 9; ++i)
        if (slopes[(std::size_t) i] >= d) return (float) i / 8.0f;
    return 1.0f;
}
double outputDb (float v) noexcept { return ((double) v - 0.5) * 48.0; }
double gainScale (float v) noexcept { return (double) v * 2.0; }

const char* shapeName (Shape s) noexcept
{
    static const char* names[] = { "Off", "Bell", "Low Shelf", "High Shelf", "Low Cut", "High Cut", "Notch", "Band Pass", "Tilt Shelf",
                                   "Flat Tilt", "All Pass" };
    return names[(int) s];
}

const EffectInfo& info()
{
    static const EffectInfo i = [] {
        EffectInfo e { "Facet EQ", {} };
        static std::array<std::array<char, 24>, bands * fieldsPerBand> names {};
        const char* fields[] = { "Shape", "Freq", "Gain", "Q", "Slope" };
        for (int b = 0; b < bands; ++b)
            for (int f = 0; f < fieldsPerBand; ++f)
            {
                auto& n = names[(std::size_t) param (b, f)];
                std::snprintf (n.data(), n.size(), "%d %s", b + 1, fields[f]);
                const float def = f == 0 ? 0.0f : f == 1 ? hzValue (std::array<double, 8> { 50, 120, 300, 700, 1500, 3500, 8000, 15000 }[(std::size_t) b])
                                : f == 2 ? 0.5f : f == 3 ? 0.5f : slopeValue (12);
                e.params.push_back ({ n.data(), def });
            }
        e.params.push_back ({ "Output", 0.5f });
        e.params.push_back ({ "Gain Scale", 0.5f });
        static std::array<std::array<char, 24>, bands * 2> dynNames {};
        for (int k = 0; k < bands * 2; ++k)
        {
            auto& n = dynNames[(std::size_t) k];
            std::snprintf (n.data(), n.size(), k < bands ? "%d Range" : "%d Threshold", k % bands + 1);
            e.params.push_back ({ n.data(), k < bands ? 0.5f : thresholdValue (-20.0) });
        }
        e.params.push_back ({ "Attack", 0.5f });
        e.params.push_back ({ "Release", 0.5f });
        e.params.push_back ({ "Phase", 0.0f });
        e.params.push_back ({ "Solo", 0.0f });
        e.params.push_back ({ "Detector", 0.0f });
        e.params.push_back ({ "Analyzer", 1.0f });
        // round 128 (Pro-Q parity): band power and stereo placement, output options, display settings
        static std::array<std::array<char, 24>, bands * 2> bandExtras {};
        for (int k = 0; k < bands * 2; ++k)
        {
            auto& n = bandExtras[(std::size_t) k];
            std::snprintf (n.data(), n.size(), k < bands ? "%d Bypass" : "%d Placement", k % bands + 1);
            e.params.push_back ({ n.data(), 0.0f });
        }
        e.params.push_back ({ "Auto Gain", 0.0f });
        e.params.push_back ({ "Invert", 0.0f });
        e.params.push_back ({ "Pan", 0.5f });
        e.params.push_back ({ "Pan Mode", 0.0f });
        e.params.push_back ({ "Analyzer Range", 0.5f });
        e.params.push_back ({ "Analyzer Speed", 0.5f });
        e.params.push_back ({ "Analyzer Tilt", 0.75f });
        e.params.push_back ({ "Display Range", displayRangeValue (12.0) });
        e.params.push_back ({ "Character", 0.0f }); // round 135
        e.params.push_back ({ "Resolution", 0.25f }); // round 144: Medium (the old FIR length)
        // round 152: spectral dynamics per band (off = the old dynamic band), density, tilt
        static std::array<std::array<char, 24>, bands * 3> specNames {};
        for (int k = 0; k < bands * 3; ++k)
        {
            auto& n = specNames[(std::size_t) k];
            std::snprintf (n.data(), n.size(), k < bands ? "%d Spectral" : k < 2 * bands ? "%d Density" : "%d Spectral Tilt", k % bands + 1);
            e.params.push_back ({ n.data(), k >= bands && k < 2 * bands ? 0.5f : 0.0f });
        }
        e.params.push_back ({ "Listen", 0.0f });       // round 166f: the EQ's output (the old behaviour)
        e.params.push_back ({ "Target Level", 1.0f }); // level match on
        return e;
    }();
    return i;
}

std::string paramText (int p, float v)
{
    char t[48];
    if (p == outputParam) { std::snprintf (t, sizeof (t), "%+.2f dB", outputDb (v)); return t; }
    if (p == scaleParam) { std::snprintf (t, sizeof (t), "%.0f%%", gainScale (v) * 100.0); return t; }
    if (p >= rangeParam0 && p < rangeParam0 + bands) { std::snprintf (t, sizeof (t), "%+.2f dB", rangeDb (v)); return t; }
    if (p >= thresholdParam0 && p < thresholdParam0 + bands) { std::snprintf (t, sizeof (t), "%.1f dB", thresholdDb (v)); return t; }
    if (p == attackParam) { std::snprintf (t, sizeof (t), attackMs (v) < 10.0 ? "%.2f ms" : "%.1f ms", attackMs (v)); return t; }
    if (p == releaseParam) { const auto ms = releaseMs (v); std::snprintf (t, sizeof (t), ms < 1000.0 ? "%.0f ms" : "%.2f s", ms < 1000.0 ? ms : ms / 1000.0); return t; }
    if (p == phaseParam) return phaseName (phase (v));
    if (p == soloParam) { const auto b = soloBand (v); if (b < 0) return "Off"; std::snprintf (t, sizeof (t), "Band %d", b + 1); return t; }
    if (p == detectorParam) return sidechainDetector (v) ? "Sidechain" : "Input";
    if (p == analyzerParam) { static const char* a[] = { "Off", "Pre", "Post", "Pre + Post" }; return a[(int) analyzer (v)]; }
    if (p >= powerParam0 && p < powerParam0 + bands) return v >= 0.5f ? "Off" : "On";
    if (p >= placementParam0 && p < placementParam0 + bands) return placementName (placement (v));
    if (p == autoGainParam) return v >= 0.5f ? "On" : "Off";
    if (p == invertParam) return v >= 0.5f ? "Inverted" : "Normal";
    if (p == panParam)
    {
        const auto a = panAmount (v);
        if (std::abs (a) < 0.005) return "Centre";
        std::snprintf (t, sizeof (t), "%s %.0f%%", a < 0.0 ? "L" : "R", std::abs (a) * 100.0);
        return t;
    }
    if (p == panModeParam) return panMidSide (v) ? "Mid/Side" : "Left/Right";
    if (p == specRangeParam) { std::snprintf (t, sizeof (t), "%.0f dB", analyzerRangeDb (v)); return t; }
    if (p == specSpeedParam) { static const char* a[] = { "Very slow", "Slow", "Medium", "Fast", "Very fast" }; return a[analyzerSpeed (v)]; }
    if (p == specTiltParam) { std::snprintf (t, sizeof (t), "%.1f dB/oct", analyzerTiltDb (v)); return t; }
    if (p == displayRangeParam) { std::snprintf (t, sizeof (t), "%.0f dB", displayRangeDb (v)); return t; }
    if (p == characterParam) { static const char* a[] = { "Clean", "Subtle", "Warm" }; return a[character (v)]; }
    if (p == resolutionParam) return resolutionName (resolution (v));
    if (p >= spectralParam0 && p < spectralParam0 + bands) return v >= 0.5f ? "Spectral" : "Off";
    if (p >= densityParam0 && p < densityParam0 + bands) { std::snprintf (t, sizeof (t), "%.0f%%", density (v) * 100.0); return t; }
    if (p >= spectralTiltParam0 && p < spectralTiltParam0 + bands) return v >= 0.5f ? "On" : "Off";
    if (p == listenParam) return listenName (listen (v));
    if (p == targetLevelParam) return v >= 0.5f ? "Matched" : "As is";
    switch (p % fieldsPerBand)
    {
        case 0: return shapeName (shape (v));
        case 1: { const auto f = hz (v); std::snprintf (t, sizeof (t), f < 1000 ? "%.1f Hz" : "%.2f kHz", f < 1000 ? f : f / 1000.0); return t; }
        case 2: std::snprintf (t, sizeof (t), "%+.2f dB", gainDb (v)); return t;
        case 3: std::snprintf (t, sizeof (t), "%.3f", q (v)); return t;
        default: std::snprintf (t, sizeof (t), "%d dB/oct", slopeDb (v)); return t;
    }
}

int design (const EffectParams& params, int band, double sampleRate, std::array<Section, maxSections>& out, double extraDb) noexcept
{
    Analog a[maxSections];
    const int n = analogSections (params, band, a, extraDb);
    const double f0 = std::min (hz (params[(std::size_t) param (band, 1)]), 0.4999 * sampleRate);
    const double w0 = 2.0 * pi * f0 / sampleRate;
    for (int k = 0; k < n; ++k)
        out[(std::size_t) k] = matched (a[k], w0);
    return n;
}

double analogDb (const EffectParams& params, int band, double f, double extraDb) noexcept
{
    Analog a[maxSections];
    const int n = analogSections (params, band, a, extraDb);
    const double w = f / hz (params[(std::size_t) param (band, 1)]);
    double m = 1.0;
    for (int k = 0; k < n; ++k) m *= std::sqrt (mag2 (a[k], w));
    return 20.0 * std::log10 (std::max (m, 1e-15));
}

double bandResponseDb (const EffectParams& params, int band, double sampleRate, double f, double extraDb) noexcept
{
    if (phase (params[(std::size_t) phaseParam]) != Phase::zeroLatency)
        return analogDb (params, band, f, extraDb);
    std::array<Section, maxSections> s;
    const int n = design (params, band, sampleRate, s, extraDb);
    const double w = 2.0 * pi * std::min (f, 0.5 * sampleRate) / sampleRate;
    double m = 1.0;
    for (int k = 0; k < n; ++k) m *= std::abs (evalDigital (s[(std::size_t) k], w));
    return 20.0 * std::log10 (std::max (m, 1e-15));
}

void bandCurveDb (const EffectParams& params, int band, double sampleRate, const double* f, int n, double* out, double extraDb) noexcept
{
    if (phase (params[(std::size_t) phaseParam]) != Phase::zeroLatency)
    {
        Analog a[maxSections];
        const int c = analogSections (params, band, a, extraDb);
        const double f0 = hz (params[(std::size_t) param (band, 1)]);
        for (int i = 0; i < n; ++i)
        {
            double m = 1.0;
            for (int k = 0; k < c; ++k) m *= mag2 (a[k], f[i] / f0);
            out[i] = 10.0 * std::log10 (std::max (m, 1e-30));
        }
        return;
    }
    std::array<Section, maxSections> s;
    const int c = design (params, band, sampleRate, s, extraDb);
    for (int i = 0; i < n; ++i)
    {
        const double w = 2.0 * pi * std::min (f[i], 0.5 * sampleRate) / sampleRate;
        double m = 1.0;
        for (int k = 0; k < c; ++k) m *= std::abs (evalDigital (s[(std::size_t) k], w));
        out[i] = 20.0 * std::log10 (std::max (m, 1e-15));
    }
}

double responseDb (const EffectParams& params, double sampleRate, double f, const float* dynamicDb) noexcept
{
    double db = outputDb (params[(std::size_t) outputParam]);
    for (int b = 0; b < bands; ++b)
        db += bandResponseDb (params, b, sampleRate, f, dynamicDb != nullptr && isDynamic (params, b) ? (double) dynamicDb[b] : 0.0);
    return db;
}

bool hasGain (Shape s) noexcept
{
    return s == Shape::bell || s == Shape::lowShelf || s == Shape::highShelf || s == Shape::tiltShelf || s == Shape::flatTilt;
}
double rangeDb (float v) noexcept { return ((double) std::clamp (v, 0.0f, 1.0f) - 0.5) * 60.0; }
float rangeValue (double db) noexcept { return (float) std::clamp (db / 60.0 + 0.5, 0.0, 1.0); }
double thresholdDb (float v) noexcept { return -80.0 + 80.0 * (double) std::clamp (v, 0.0f, 1.0f); }
float thresholdValue (double db) noexcept { return (float) std::clamp ((db + 80.0) / 80.0, 0.0, 1.0); }
double attackMs (float v) noexcept { return 0.25 * std::pow (200.0, (double) std::clamp (v, 0.0f, 1.0f)); }
double releaseMs (float v) noexcept { return 20.0 * std::pow (200.0, (double) std::clamp (v, 0.0f, 1.0f)); }
Phase phase (float v) noexcept { return (Phase) std::clamp ((int) std::lround (v * (phaseCount - 1)), 0, phaseCount - 1); }
float phaseValue (Phase p) noexcept { return (float) (int) p / (float) (phaseCount - 1); }
int soloBand (float v) noexcept { return std::clamp ((int) std::lround (v * bands), 0, bands) - 1; }
float soloValue (int band) noexcept { return band < 0 ? 0.0f : (float) (band + 1) / (float) bands; }
bool sidechainDetector (float v) noexcept { return v >= 0.5f; }
Analyzer analyzer (float v) noexcept { return (Analyzer) std::clamp ((int) std::lround (v * 3.0f), 0, 3); }
float analyzerValue (Analyzer a) noexcept { return (float) (int) a / 3.0f; }
const char* phaseName (Phase p) noexcept
{
    static const char* names[] = { "Zero Latency", "Natural Phase", "Linear Phase" };
    return names[(int) p];
}

bool isDynamic (const EffectParams& p, int b) noexcept
{
    return hasGain (shape (p[(std::size_t) param (b, 0)])) && bandPower (p, b) && std::abs (rangeDb (p[(std::size_t) (rangeParam0 + b)])) > 0.01;
}

GraphHit pickGraphTarget (const float* nodeX, const float* nodeY, const bool* on, const bool* dynamic, const float* rangeX,
                          const float* rangeY, int count, int selectedBand, float px, float py, float nodeRadius, float rangeRadius) noexcept
{
    auto distance = [px, py] (float x, float y) { return std::hypot (x - px, y - py); };
    GraphHit hit;
    // 1. a node: the nearest one within its radius always wins (moving a band is what a press near it means)
    float best = nodeRadius;
    for (int b = 0; b < count; ++b)
        if (on[b])
            if (const auto d = distance (nodeX[b], nodeY[b]); d < best) { best = d; hit = { b, false }; }
    if (hit.band >= 0) return hit;
    // 2. the range handle of a dynamic band, a small target of its own (the selected band's first on a tie)
    best = rangeRadius;
    for (int b = 0; b < count; ++b)
        if (on[b] && dynamic[b])
            if (const auto d = distance (rangeX[b], rangeY[b]) - (b == selectedBand ? 0.01f : 0.0f); d < best) { best = d; hit = { b, true }; }
    return hit;
}

Listen listen (float v) noexcept { return v < 0.25f ? Listen::eq : v < 0.75f ? Listen::source : Listen::target; }
float listenValue (Listen l) noexcept { return l == Listen::eq ? 0.0f : l == Listen::source ? 0.5f : 1.0f; }
const char* listenName (Listen l) noexcept { return l == Listen::eq ? "EQ" : l == Listen::source ? "Source" : "Target"; }
bool listensToTarget (const EffectParams& p) noexcept { return listen (p[(std::size_t) listenParam]) == Listen::target; }

double targetMatchGain (double sourcePower, double targetPower) noexcept
{
    constexpr double floor = 1e-10; // -100 dB: silence on either side keeps unity
    if (! (sourcePower > floor) || ! (targetPower > floor)) return 1.0;
    return std::clamp (std::sqrt (sourcePower / targetPower), 0.0630957, 15.8489); // ±24 dB
}

bool isSpectral (const EffectParams& p, int b) noexcept
{
    return isDynamic (p, b) && p[(std::size_t) (spectralParam0 + b)] >= 0.5f;
}
double density (float v) noexcept { return std::clamp ((double) v, 0.0, 1.0); }
int spectralLatency (double sampleRate) noexcept { return 5120 * rateFactor (sampleRate); }

void spectralGainsDb (const EffectParams& p, int b, double sampleRate, const double* f, const double* levelDb, int n, double* out) noexcept
{
    (void) sampleRate;
    std::vector<double> W ((std::size_t) n), S ((std::size_t) n);
    double ref = 0.0;
    const auto q = shareParams (p, b, ref);
    const double f0 = hz (p[(std::size_t) param (b, 1)]);
    const bool tilt = p[(std::size_t) (spectralTiltParam0 + b)] >= 0.5f;
    for (int i = 0; i < n; ++i)
    {
        S[(std::size_t) i] = spectralShare (q, b, f[i], ref);
        W[(std::size_t) i] = levelDb[i] + specWeightExp * 20.0 * std::log10 (std::max (S[(std::size_t) i], 1e-4)) + (tilt ? specTiltDbOct * std::log2 (f[i] / f0) : 0.0);
    }
    spectralLaw (W.data(), S.data(), n, thresholdDb (p[(std::size_t) (thresholdParam0 + b)]), rangeDb (p[(std::size_t) (rangeParam0 + b)]), out);
}

int matchEq (const EffectParams& params, const double* f, const double* cur, const double* ref, int n, double amount, int maxBands,
             double smoothingOct, std::vector<std::pair<int, float>>& changes)
{
    changes.clear();
    if (n < 8) return 0;
    // the target: reference − current, without its mean level (100 Hz … 10 kHz), smoothed in log frequency
    std::vector<double> d ((std::size_t) n), t ((std::size_t) n), lf ((std::size_t) n);
    double mean = 0.0;
    int cnt = 0;
    for (int i = 0; i < n; ++i)
    {
        d[(std::size_t) i] = ref[i] - cur[i];
        lf[(std::size_t) i] = std::log2 (std::max (1.0, f[i]));
        if (f[i] >= 100.0 && f[i] <= 10000.0) { mean += d[(std::size_t) i]; ++cnt; }
    }
    mean = cnt > 0 ? mean / cnt : 0.0;
    const double sg = std::max (0.02, smoothingOct);
    for (int i = 0; i < n; ++i)
    {
        double acc = 0.0, w = 0.0;
        for (int j = 0; j < n; ++j)
        {
            const double u = (lf[(std::size_t) j] - lf[(std::size_t) i]) / sg;
            if (std::abs (u) > 3.0) continue;
            const double k = std::exp (-0.5 * u * u);
            acc += k * (d[(std::size_t) j] - mean);
            w += k;
        }
        t[(std::size_t) i] = std::clamp (amount * acc / std::max (w, 1e-12), -18.0, 18.0);
    }
    // the free bands (shape off)
    std::vector<int> freeBands;
    for (int b = 0; b < bands; ++b)
        if (shape (params[(std::size_t) param (b, 0)]) == Shape::off) freeBands.push_back (b);
    auto work = params;
    std::vector<double> r = t;
    std::vector<int> used;
    const auto bandAt = [&] (int b, std::vector<double>& out) {
        for (int i = 0; i < n; ++i) out[(std::size_t) i] = analogDb (work, b, f[i]);
    };
    std::vector<double> resp ((std::size_t) n);
    for (const int b : freeBands)
    {
        if ((int) used.size() >= maxBands) break;
        int peak = -1;
        double best = 0.0;
        for (int i = 0; i < n; ++i)
            if (f[i] >= 25.0 && f[i] <= 18000.0 && std::abs (r[(std::size_t) i]) > best) { best = std::abs (r[(std::size_t) i]); peak = i; }
        if (peak < 0 || best < 0.5) break;
        const double g = r[(std::size_t) peak];
        // the half-gain width around the peak
        int lo = peak, hi = peak;
        while (lo > 0 && r[(std::size_t) (lo - 1)] * g > 0.0 && std::abs (r[(std::size_t) (lo - 1)]) > 0.5 * std::abs (g)) --lo;
        while (hi < n - 1 && r[(std::size_t) (hi + 1)] * g > 0.0 && std::abs (r[(std::size_t) (hi + 1)]) > 0.5 * std::abs (g)) ++hi;
        Shape sh = Shape::bell;
        double fc = f[peak], qv = 1.0;
        if (lo == 0 && f[peak] < 120.0) { sh = Shape::lowShelf; fc = f[hi]; qv = 0.7; }
        else if (hi == n - 1 && f[peak] > 6000.0) { sh = Shape::highShelf; fc = f[lo]; qv = 0.7; }
        else
        {
            const double bw = std::max (0.1, lf[(std::size_t) hi] - lf[(std::size_t) lo]);
            qv = std::clamp (1.0 / (2.0 * std::sinh (std::log (2.0) / 2.0 * bw)), 0.3, 8.0);
        }
        work[(std::size_t) param (b, 0)] = shapeValue (sh);
        work[(std::size_t) param (b, 1)] = hzValue (std::clamp (fc, 20.0, 20000.0));
        work[(std::size_t) param (b, 2)] = gainValue (g);
        work[(std::size_t) param (b, 3)] = qValue (qv);
        work[(std::size_t) param (b, 4)] = slopeValue (12);
        work[(std::size_t) (rangeParam0 + b)] = 0.5f;
        work[(std::size_t) (powerParam0 + b)] = 0.0f;
        bandAt (b, resp);
        for (int i = 0; i < n; ++i) r[(std::size_t) i] -= resp[(std::size_t) i];
        used.push_back (b);
    }
    // two refinement passes: each band's gain takes the residual at its frequency
    for (int pass = 0; pass < 3; ++pass)
        for (const int b : used)
        {
            const double fb = hz (work[(std::size_t) param (b, 1)]);
            int k = 0;
            for (int i = 1; i < n; ++i) if (std::abs (f[i] - fb) < std::abs (f[k] - fb)) k = i;
            bandAt (b, resp);
            for (int i = 0; i < n; ++i) r[(std::size_t) i] += resp[(std::size_t) i];
            const auto sh = shape (work[(std::size_t) param (b, 0)]);
            const double target = sh == Shape::lowShelf ? r[0] : sh == Shape::highShelf ? r[(std::size_t) (n - 1)] : r[(std::size_t) k];
            const double now = gainDb (work[(std::size_t) param (b, 2)]);
            const double ref0 = sh == Shape::bell ? resp[(std::size_t) k] : (sh == Shape::lowShelf ? resp[0] : resp[(std::size_t) (n - 1)]);
            const double gnew = std::clamp (now + 0.8 * (target - ref0), -30.0, 30.0);
            work[(std::size_t) param (b, 2)] = gainValue (gnew);
            bandAt (b, resp);
            for (int i = 0; i < n; ++i) r[(std::size_t) i] -= resp[(std::size_t) i];
        }
    for (const int b : used)
        for (const int prm : { param (b, 0), param (b, 1), param (b, 2), param (b, 3), param (b, 4), rangeParam0 + b, powerParam0 + b })
            changes.push_back ({ prm, work[(std::size_t) prm] });
    return (int) used.size();
}

double dynamicGainDb (double level, double threshold, double range) noexcept
{
    const double x = level - threshold;
    constexpr double knee = 10.0; // fitted to the reference (REA_rodada103 §2.1)
    const double over = x <= -0.5 * knee ? 0.0 : x < 0.5 * knee ? (x + 0.5 * knee) * (x + 0.5 * knee) / (2.0 * knee) : x;
    return range < 0.0 ? -std::min (-range, over) : std::min (range, over);
}

int resolution (float v) noexcept { return std::clamp ((int) std::lround (std::clamp (v, 0.0f, 1.0f) * 4.0f), 0, resolutionCount - 1); }
float resolutionValue (int r) noexcept { return (float) std::clamp (r, 0, resolutionCount - 1) / 4.0f; }
const char* resolutionName (int r) noexcept
{
    static const char* names[] = { "Low", "Medium", "High", "Very High", "Maximum" };
    return names[std::clamp (r, 0, resolutionCount - 1)];
}

int linearHalfTaps (int r, double sampleRate) noexcept
{
    static constexpr int half[resolutionCount] = { 2048, 4096, 8192, 16384, 65536 }; // [EXEC] the trial: no 32768
    return half[std::clamp (r, 0, resolutionCount - 1)] * rateFactor (sampleRate);
}

int latencyFor (Phase p, double sampleRate, int r) noexcept
{
    const int f = rateFactor (sampleRate);
    switch (p)
    {
        case Phase::natural: return (naturalBlock + naturalDelay) * f;
        case Phase::linear: return linearBlock * f + linearHalfTaps (r, sampleRate);
        default: return 0;
    }
}

Placement placement (float v) noexcept { return (Placement) std::clamp ((int) std::lround (v * (placementCount - 1)), 0, placementCount - 1); }
float placementValue (Placement p) noexcept { return (float) (int) p / (float) (placementCount - 1); }
const char* placementName (Placement p) noexcept
{
    static const char* names[] = { "Stereo", "Left", "Right", "Mid", "Side" };
    return names[(int) p];
}
bool bandPower (const EffectParams& p, int b) noexcept { return p[(std::size_t) (powerParam0 + b)] < 0.5f; } // stored as "bypassed" so a zeroed array is on
double panAmount (float v) noexcept { return std::clamp ((double) v * 2.0 - 1.0, -1.0, 1.0); }
bool panMidSide (float v) noexcept { return v >= 0.5f; }
double analyzerRangeDb (float v) noexcept { return 60.0 + 30.0 * std::clamp ((int) std::lround (v * 2.0f), 0, 2); }
int analyzerSpeed (float v) noexcept { return std::clamp ((int) std::lround (v * 4.0f), 0, 4); }
double analyzerTiltDb (float v) noexcept { return 1.5 * std::clamp ((int) std::lround (v * 4.0f), 0, 4); }
double displayRangeDb (float v) noexcept
{
    static constexpr double r[] { 3.0, 6.0, 12.0, 30.0 };
    return r[std::clamp ((int) std::lround (v * 3.0f), 0, 3)];
}
float displayRangeValue (double db) noexcept { return db <= 4.5 ? 0.0f : db <= 9.0 ? 1.0f / 3.0f : db <= 21.0 ? 2.0f / 3.0f : 1.0f; }

double autoGainDb (const EffectParams& params, double sampleRate) noexcept
{
    constexpr int points = 32;
    double sum = 0.0;
    for (int b = 0; b < bands; ++b)
    {
        Analog a[maxSections];
        const int c = analogSections (params, b, a);
        if (c == 0) continue;
        const double f0 = hz (params[(std::size_t) param (b, 1)]);
        for (int i = 0; i < points; ++i)
        {
            const double f = std::min (20.0 * std::pow (1000.0, (i + 0.5) / points), 0.49 * sampleRate);
            double m = 1.0;
            for (int k = 0; k < c; ++k) m *= mag2 (a[k], f / f0);
            sum += 10.0 * std::log10 (std::max (m, 1e-30));
        }
    }
    return sum / points;
}

std::unique_ptr<Effect> make (const EffectParams* initial) { return std::make_unique<FacetEqEffect> (initial); }
} // namespace lynx::facet
