#include "core/FlAutoSlicer.h"

#include "core/FlResampler.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstring>

#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC optimize ("fp-contract=off")
#endif

namespace lynx::flslice
{
namespace
{
#if defined(__x86_64__) || defined(__i386__)
using ext = long double; // the plugin's x87 filter keeps its state in extended precision
#else
using ext = double;
#endif

inline std::int64_t rnd (double x) noexcept { return (std::int64_t) std::nearbyint (x); } // Delphi Round (half to even)

float bitsToFloat (std::uint32_t u) noexcept
{
    float f;
    std::memcpy (&f, &u, 4);
    return f;
}

constexpr double analysisRate = 44100.0;
constexpr int frameSize = 128, hop = 16;

// the analysis window (type 3 of the library's window generator): 0.53836 − 0.46164·cos(2πn/127), and 1/Σw
struct Window
{
    std::array<float, frameSize> w {};
    float scale = 0.0f;
    Window()
    {
        double sum = 0.0;
        for (int n = 0; n < frameSize; ++n)
        {
            w[(std::size_t) n] = (float) (0.53836 - std::cos (6.283185307179586 / 127.0 * n) * 0.46164);
            sum += (double) w[(std::size_t) n];
        }
        scale = 1.0f / (float) sum;
    }
};

const Window& window()
{
    static const Window w;
    return w;
}

// radix-2 FFT of 128 real values; |X[k]| for k = 1 … 63
void magnitudes (const float* x, float* mag)
{
    std::array<std::complex<double>, frameSize> a;
    for (int i = 0; i < frameSize; ++i)
    {
        int r = 0;
        for (int b = 0; b < 7; ++b)
            r |= ((i >> b) & 1) << (6 - b);
        a[(std::size_t) r] = (double) x[i];
    }
    static const auto tw = [] {
        std::array<std::complex<double>, frameSize / 2> t;
        for (int k = 0; k < frameSize / 2; ++k)
            t[(std::size_t) k] = std::polar (1.0, -6.283185307179586 * k / frameSize);
        return t;
    }();
    for (int len = 2; len <= frameSize; len <<= 1)
        for (int i = 0; i < frameSize; i += len)
            for (int j = 0; j < len / 2; ++j)
            {
                const auto t = tw[(std::size_t) (j * (frameSize / len))] * a[(std::size_t) (i + j + len / 2)];
                const auto u = a[(std::size_t) (i + j)];
                a[(std::size_t) (i + j)] = u + t;
                a[(std::size_t) (i + j + len / 2)] = u - t;
            }
    for (int k = 1; k < 64; ++k)
        mag[k - 1] = (float) std::abs (a[(std::size_t) k]);
}

// FL's x87 control word in the library's sample reader: 64-bit precision, rounding toward −∞
#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
struct X87Down
{
    unsigned short saved = 0;
    X87Down() noexcept
    {
        asm volatile ("fnstcw %0" : "=m"(saved));
        unsigned short cw = (unsigned short) ((saved & ~0xc00) | 0x400 | 0x300);
        asm volatile ("fldcw %0" : : "m"(cw) : "memory");
    }
    ~X87Down() { asm volatile ("fldcw %0" : : "m"(saved) : "memory"); }
};
#else
struct X87Down
{
};
#endif

// the copy at 44.1 kHz: the library's sample reader at its "6-point Hermite" quality, speed = rate / 44100, in blocks of
// 1024 frames (careful path near the edges with the edge frames repeated, eight-lane positions elsewhere)
std::vector<float> resampled (const std::vector<float>& x, double rate)
{
    const auto ratio = analysisRate / rate;
    const auto n = (std::int64_t) x.size();
    const auto outLen = rnd ((double) n * ratio);
    std::vector<float> out ((std::size_t) std::max<std::int64_t> (outLen, 0));
    if (n <= 0 || outLen <= 0)
        return out;
    flrs::Kernel k;
    k.setQuality (2);
    const auto speed = 1.0 / ratio;
    constexpr int pts = 3;
    const std::int64_t c0 = pts - 1, c8 = n - pts, d0 = c0 + 3, d8 = c8 - 3;
    auto frame = [&] (std::int64_t i) { return x[(std::size_t) std::clamp<std::int64_t> (i, 0, n - 1)]; };
    float taps[2 * pts];
    auto interp = [&] (std::int64_t i, double fr) {
        for (int t = 0; t < 2 * pts; ++t)
            taps[t] = frame (i - pts + 1 + t);
        float l, r;
        flrs::interpolate (k, taps, taps, fr, l, r);
        return l;
    };
    X87Down guard;
    double pos = 0.0;
    std::int64_t done = 0;
    bool ended = false;
    while (done < outLen && ! ended)
    {
        const auto block = (int) std::min<std::int64_t> (outLen - done, 1024);
        int got = 0;
        while (got < block)
        {
            const auto m = block - got;
            const auto ip = rnd (pos);
            if (ip < d0 || (double) d8 <= pos + speed * (double) m)
            {
                int q = 0;
                while (q < m)
                {
                    const auto i = (std::int64_t) std::trunc (pos);
                    if (n <= i)
                    {
                        ended = true;
                        break;
                    }
                    const auto fr = pos - (double) i;
                    pos = pos + speed;
                    const auto edge = i < c0 || c8 <= i;
                    out[(std::size_t) (done + got + q)] = interp (i, fr);
                    ++q;
                    if (edge && q < m)
                        break;
                }
                got += q;
                if (ended)
                    break;
            }
            else
            {
                double lanes[8];
                for (int l = 0; l < 8; ++l)
                    lanes[l] = pos + speed * (double) l;
                const auto full = (m / 8) * 8;
                auto at = [&] (int q, double p) {
                    const auto i = (std::int64_t) p;
                    out[(std::size_t) (done + got + q)] = interp (i, p - (double) i);
                };
                for (int b0 = 0; b0 < full; b0 += 8)
                    for (int l = 0; l < 8; ++l)
                    {
                        at (b0 + l, lanes[l]);
                        lanes[l] = lanes[l] + speed * 8.0;
                    }
                auto p = lanes[0];
                for (int q = full; q < m; ++q)
                {
                    at (q, p);
                    p = p + speed;
                }
                pos = pos + speed * (double) m;
                got += m;
            }
        }
        done += got;
    }
    return out;
}

// the four passes of the library's x87 high-pass (0x60b200): v += (x − lp)·k; lp += v; v ·= d; out = x − lp
void highPass (std::vector<float>& y)
{
    const ext k = (ext) bitsToFloat (0x38bf8dcau), d = (ext) bitsToFloat (0x3f75c28fu);
    for (int pass = 0; pass < 4; ++pass)
    {
        ext lp = 0, v = 0;
        for (auto& s : y)
        {
            const ext xi = s;
            v = v + (xi - lp) * k;
            lp = lp + v;
            v = v * d;
            s = (float) (xi - lp);
        }
    }
}

// 0x740630: peak hold over `hold` frames with an exponential release, then the rise over the previous frame
std::vector<float> onsetStrength (const std::vector<float>& band, float threshold, float decay, float gain, int hold)
{
    const auto nf = (int) band.size();
    std::vector<float> dst ((std::size_t) nf, 0.0f);
    float prev = 0.0f;
    const float keep = 1.0f - decay;
    for (int i = 0; i < nf; ++i)
    {
        const auto v = band[(std::size_t) i];
        const auto m = std::min (nf - i - 1, hold);
        for (int j = 1; j <= m; ++j)
            dst[(std::size_t) (i + j)] = std::max (dst[(std::size_t) (i + j)], v);
        auto e = std::max (dst[(std::size_t) i], v);
        if (e <= prev)
            e = prev * decay + e * keep;
        dst[(std::size_t) i] = e;
        prev = e;
    }
    const auto floorLevel = (float) std::exp ((double) (0.115129255f * -200.0f)); // −200 dB
    const auto base = floorLevel / threshold;
    auto last = base;
    for (int i = 0; i < nf; ++i)
    {
        const auto x = dst[(std::size_t) i];
        dst[(std::size_t) i] = x < last ? 0.0f : ((x / last - 1.0f) * gain) * 5.0f;
        last = std::max (x, base);
    }
    return dst;
}

int sign (float x) noexcept
{
    if ((double) std::fabs (x) >= (double) 6.103515625e-05f)
        return x < 0.0f ? -1 : 1;
    return 0;
}

// FUN_006057c0 (search back from q to lo): mono — the nearest sign change, else the quietest sample
std::int64_t zeroCrossMono (const float* d, std::int64_t q, std::int64_t lo)
{
    bool found = false;
    std::int64_t best = 0, minIdx = 0;
    float minAbs = 2147483648.0f;
    auto s = sign (d[q]);
    if (std::fabs (d[q]) < minAbs)
        minAbs = std::fabs (d[q]), minIdx = 0;
    std::int64_t cur = -1;
    for (std::int64_t it = 0; it < q - lo; ++it)
    {
        const auto v = d[q + cur];
        const auto ns = sign (v);
        if (std::fabs (v) < minAbs)
            minAbs = std::fabs (v), minIdx = cur;
        if (ns == -s || s == 0)
        {
            best = cur + 1;
            found = true;
            break;
        }
        s = ns;
        --cur;
    }
    return q + (found ? best : minIdx);
}

// stereo: a change on both channels wins, then one on a single channel, else the quietest frame
std::int64_t zeroCrossStereo (const float* L, const float* R, std::int64_t q, std::int64_t lo)
{
    bool one = false, both = false;
    std::int64_t oneAt = 0, bothAt = 0, minIdx = 0;
    float minRms = 2147483648.0f;
    auto rms = [&] (std::int64_t i) {
        const auto e = ((0.0f + L[i] * L[i]) + R[i] * R[i]) * 0.5f;
        return (float) std::sqrt ((double) e);
    };
    int sl = sign (L[q]), sr = sign (R[q]);
    if (rms (q) < minRms)
        minRms = rms (q), minIdx = 0;
    std::int64_t cur = -1;
    for (std::int64_t it = 0; it < q - lo; ++it)
    {
        const int nl = sign (L[q + cur]), nr = sign (R[q + cur]);
        const auto r = rms (q + cur);
        if (r < minRms)
            minRms = r, minIdx = cur;
        const int cnt = (nl == -sl || sl == 0 ? 1 : 0) + (nr == -sr || sr == 0 ? 1 : 0);
        sl = nl, sr = nr;
        if (cnt > 0)
        {
            const auto c = cur + 1;
            if (cnt == 2)
            {
                bothAt = c, both = true;
                break;
            }
            if (! one || std::llabs (c) < std::llabs (oneAt))
                oneAt = c, one = true;
        }
        --cur;
    }
    if (both)
        return q + bothAt;
    return q + (one ? oneAt : minIdx);
}
} // namespace

std::vector<std::int64_t> autoSlice (const float* left, const float* right, std::int64_t frames, double sampleRate)
{
    std::vector<std::int64_t> slices;
    if (left == nullptr || frames <= 0)
        return slices;
    const bool stereo = right != nullptr && right != left;
    const int ch = stereo ? 2 : 1;

    // the normalised mono copy (peak of the interleaved data; mono = (L + R)·0.5)
    float mn = left[0], mx = left[0];
    for (std::int64_t i = 0; i < frames; ++i)
    {
        mn = std::min (mn, left[i]), mx = std::max (mx, left[i]);
        if (stereo)
            mn = std::min (mn, right[i]), mx = std::max (mx, right[i]);
    }
    const auto peak = std::fabs (mn) <= std::fabs (mx) ? std::fabs (mx) : std::fabs (mn);
    const auto g = peak > 0.0f ? 1.0f / peak : 1.0f;
    std::vector<float> mono ((std::size_t) frames);
    for (std::int64_t i = 0; i < frames; ++i)
    {
        auto l = left[i];
        if (peak > 0.0f)
            l = l * g;
        if (stereo)
        {
            auto r = right[i];
            if (peak > 0.0f)
                r = r * g;
            mono[(std::size_t) i] = (l + r) * 0.5f;
        }
        else
            mono[(std::size_t) i] = l;
    }
    if (sampleRate != analysisRate)
        mono = resampled (mono, sampleRate);
    const auto ratio = (float) (sampleRate / analysisRate);
    highPass (mono);

    // band levels per hop
    const auto n = (std::int64_t) mono.size();
    const auto nf = (int) ((n + 0x8f) >> 4);
    if (nf <= 0)
        return slices;
    mono.resize ((std::size_t) std::max<std::int64_t> (n, (std::int64_t) (nf - 1) * hop + 0x80), 0.0f);
    std::vector<float> low ((std::size_t) nf), high ((std::size_t) nf);
    const auto& W = window();
    float seg[frameSize], mag[63];
    for (int f = 0; f < nf; ++f)
    {
        for (int i = 0; i < frameSize; ++i)
            seg[i] = mono[(std::size_t) (f * hop + i)] * W.w[(std::size_t) i];
        magnitudes (seg, mag);
        for (auto& m : mag)
        {
            m = m * W.scale;
            m = m * m;
        }
        double s0 = 0.0, s1 = 0.0;
        for (int k = 0; k < 5; ++k)
            s0 += (double) mag[k];
        for (int k = 5; k < 63; ++k)
            s1 += (double) mag[k];
        low[(std::size_t) f] = (float) std::sqrt ((double) ((float) s0 / 5.0f));
        high[(std::size_t) f] = (float) std::sqrt ((double) ((float) s1 / 58.0f));
    }

    // onset strength: the slicer's settings as the Granulizer sets them (gains 2)
    const float framesPerMs = 2.75625f;
    const auto d0 = (float) std::exp ((double) (-1.0f / ((framesPerMs * 1.0f) * 15.0f)));
    const auto d1 = (float) std::exp ((double) (-1.0f / ((framesPerMs * 1.0f) * 10.0f)));
    const auto e0 = onsetStrength (low, 1.8f, d0, (float) ((double) 2.0f * 0.9), (int) rnd ((double) (framesPerMs * 20.0f)));
    const auto e1 = onsetStrength (high, bitsToFloat (0x41a2c235u), d1, 2.0f, (int) rnd ((double) (framesPerMs * 15.0f)));
    std::vector<float> on ((std::size_t) nf);
    for (int i = 0; i < nf; ++i)
        on[(std::size_t) i] = std::max (e0[(std::size_t) i], e1[(std::size_t) i]);

    // picking, refined on the original sample
    const auto minDist = rnd (60.0 * sampleRate * 0.001), back = rnd (20.0 * sampleRate * 0.001);
    const auto threshold = (float) ((double) peak * 0.05);
    auto level = [&] (std::int64_t q) {
        float s = 0.0f;
        s = s + left[q] * left[q];
        if (stereo)
            s = s + right[q] * right[q];
        return (float) std::sqrt ((double) (s / (float) ch));
    };
    int i = 0;
    while (i < nf)
    {
        if (on[(std::size_t) i] < 1.0f)
        {
            ++i;
            continue;
        }
        auto k = i;
        for (int q = i; q >= std::max (i - 28, 0); --q)
            if (on[(std::size_t) q] > 0.75f)
                k = q;
        const auto centre = k * hop + 64;
        auto p0 = rnd ((double) ((float) (centre - 64) * ratio));
        p0 = std::clamp<std::int64_t> (p0, 0, frames - 1);
        std::int64_t at = 0;
        if (p0 >= 1)
        {
            auto q = p0;
            while (q < frames - 1 && ! (threshold < level (q)))
                ++q;
            const auto lo = std::max<std::int64_t> (q - back, 0);
            at = stereo ? zeroCrossStereo (left, right, q, lo) : zeroCrossMono (left, q, lo);
        }
        if (at <= frames - minDist)
            slices.push_back (at);
        const auto nextFrame = rnd ((double) (rnd ((double) ((float) (at + minDist) / ratio)) + 64) / 16.0);
        i = (int) std::max<std::int64_t> (nextFrame, i + 1);
    }
    return slices;
}
} // namespace lynx::flslice
