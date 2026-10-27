#include "core/AudioEditTools.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <numeric>
#include <tuple>
#include <utility>

namespace lynx::audioedit
{
namespace
{
constexpr double pi = 3.141592653589793;

double clamp01 (double v) noexcept { return std::clamp (v, 0.0, 1.0); }

double dbToGain (double db) noexcept { return std::pow (10.0, db / 20.0); }

// a small deterministic generator (xorshift32) for the noise tools
struct Rng
{
    std::uint32_t s;
    explicit Rng (std::uint32_t seed) : s (seed == 0 ? 0x9e3779b9u : seed) {}
    std::uint32_t next() noexcept
    {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    double uniform() noexcept { return (double) next() / 4294967296.0; }   // 0..1
    double bipolar() noexcept { return uniform() * 2.0 - 1.0; }            // -1..1
};

void report (Progress* p, double f)
{
    if (p != nullptr)
        p->set ((float) clamp01 (f));
}

bool stop (Progress* p) { return p != nullptr && p->cancelled(); }

// --- STFT -------------------------------------------------------------------------------------------------------
// Overlap-add STFT with a periodic sqrt-Hann window on analysis and synthesis (hop = size / 4: the summed
// window^2 is constant = 1.5, divided out). `fn` gets the spectrum of each frame (bins 0..size/2, the rest is
// rebuilt as the conjugate mirror) and may change it. Frames are centred so the whole input is covered.
using SpectrumFn = std::function<void (std::vector<double>& re, std::vector<double>& im, std::int64_t frame)>;

std::vector<double> sqrtHann (int n)
{
    std::vector<double> w ((std::size_t) n);
    for (int i = 0; i < n; ++i)
        w[(std::size_t) i] = std::sqrt (0.5 - 0.5 * std::cos (2.0 * pi * i / n));
    return w;
}

std::vector<float> stftProcess (const std::vector<float>& in, int size, const SpectrumFn& fn, Progress* progress = nullptr,
                                double progressFrom = 0.0, double progressSpan = 1.0)
{
    const auto n = (std::int64_t) in.size();
    const int hop = size / 4;
    const auto window = sqrtHann (size);
    std::vector<double> acc ((std::size_t) (n + 2 * size), 0.0);
    std::vector<double> re ((std::size_t) size), im ((std::size_t) size);
    const auto frames = (n + size) / hop + 1;
    const auto half = size / 2;

    for (std::int64_t f = 0; f < frames; ++f)
    {
        if ((f & 63) == 0)
        {
            if (stop (progress))
                return in;
            report (progress, progressFrom + progressSpan * (double) f / (double) frames);
        }
        const auto first = f * hop - size; // input index of window sample 0
        for (int i = 0; i < size; ++i)
        {
            const auto k = first + i;
            re[(std::size_t) i] = (k >= 0 && k < n ? (double) in[(std::size_t) k] : 0.0) * window[(std::size_t) i];
            im[(std::size_t) i] = 0.0;
        }
        fftInPlace (re, im, false);
        fn (re, im, f);
        for (int b = 1; b < half; ++b) // keep the spectrum real-signal symmetric
        {
            re[(std::size_t) (size - b)] = re[(std::size_t) b];
            im[(std::size_t) (size - b)] = -im[(std::size_t) b];
        }
        im[0] = 0.0;
        im[(std::size_t) half] = 0.0;
        fftInPlace (re, im, true);
        for (int i = 0; i < size; ++i)
        {
            const auto k = first + i + size; // acc is offset by `size`
            if (k >= 0 && k < (std::int64_t) acc.size())
                acc[(std::size_t) k] += re[(std::size_t) i] / size * window[(std::size_t) i];
        }
    }
    std::vector<float> out ((std::size_t) n);
    for (std::int64_t i = 0; i < n; ++i)
        out[(std::size_t) i] = (float) (acc[(std::size_t) (i + size)] / 1.5);
    return out;
}

// the audio of [r.start - before, r.end + after) (zeros past the ends), one vector per channel
std::vector<std::vector<float>> excerpt (const Audio& a, std::int64_t from, std::int64_t to)
{
    std::vector<std::vector<float>> out;
    for (const auto& ch : a.channels)
    {
        std::vector<float> v ((std::size_t) std::max<std::int64_t> (0, to - from), 0.0f);
        for (auto i = std::max<std::int64_t> (from, 0); i < std::min<std::int64_t> (to, (std::int64_t) ch.size()); ++i)
            v[(std::size_t) (i - from)] = ch[(std::size_t) i];
        out.push_back (std::move (v));
    }
    return out;
}

float peakOf (const std::vector<float>& v, std::int64_t a, std::int64_t b)
{
    float p = 0.0f;
    for (auto i = a; i < b; ++i)
        p = std::max (p, std::abs (v[(std::size_t) i]));
    return p;
}

// a short raised-cosine fade over [a, b) of every channel (in: 0 -> 1)
void fadeFrames (Audio& audio, std::int64_t a, std::int64_t b, bool in)
{
    const auto len = b - a;
    if (len <= 0)
        return;
    for (auto& ch : audio.channels)
        for (auto i = a; i < b && i < (std::int64_t) ch.size(); ++i)
        {
            const auto t = ((double) (i - a) + 0.5) / (double) len;
            const auto g = 0.5 - 0.5 * std::cos (pi * (in ? t : 1.0 - t));
            ch[(std::size_t) i] *= (float) g;
        }
}

// Ramer-Douglas-Peucker on sorted points; keeps first and last
void rdp (const std::vector<EnvPoint>& p, std::size_t a, std::size_t b, double eps, std::vector<bool>& keep)
{
    if (b <= a + 1)
        return;
    double worst = -1.0;
    std::size_t at = a;
    for (auto i = a + 1; i < b; ++i)
    {
        const auto span = p[b].x - p[a].x;
        const auto t = span > 0.0 ? (p[i].x - p[a].x) / span : 0.0;
        const auto d = std::abs (p[i].y - (p[a].y + (p[b].y - p[a].y) * t));
        if (d > worst)
        {
            worst = d;
            at = i;
        }
    }
    if (worst > eps)
    {
        keep[at] = true;
        rdp (p, a, at, eps, keep);
        rdp (p, at, b, eps, keep);
    }
}

// samples of an envelope on a uniform grid of n values over x 0..1
std::vector<double> sampleEnvelope (const Envelope& e, int n)
{
    std::vector<double> v ((std::size_t) n);
    for (int i = 0; i < n; ++i)
        v[(std::size_t) i] = e.at (n > 1 ? (double) i / (n - 1) : 0.0);
    return v;
}

Envelope fromSamples (const std::vector<double>& v)
{
    Envelope e;
    const auto n = v.size();
    for (std::size_t i = 0; i < n; ++i)
        e.points.push_back ({ n > 1 ? (double) i / (double) (n - 1) : 0.0, clamp01 (v[i]), 0.0f });
    return e;
}

double decimationEpsilon (double threshold) noexcept
{
    const auto t = clamp01 (threshold);
    return t <= 0.0 ? -1.0 : 0.002 + 0.2 * t * t;
}
} // namespace

// --- FFT ---------------------------------------------------------------------------------------------------------
void fftInPlace (std::vector<double>& re, std::vector<double>& im, bool inverse)
{
    const auto n = re.size();
    for (std::size_t i = 1, j = 0; i < n; ++i)
    {
        auto bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
        {
            std::swap (re[i], re[j]);
            std::swap (im[i], im[j]);
        }
    }
    for (std::size_t len = 2; len <= n; len <<= 1)
    {
        const auto angle = 2.0 * pi / (double) len * (inverse ? 1.0 : -1.0);
        const auto wr = std::cos (angle), wi = std::sin (angle);
        for (std::size_t i = 0; i < n; i += len)
        {
            double cr = 1.0, ci = 0.0;
            for (std::size_t k = 0; k < len / 2; ++k)
            {
                const auto a = i + k, b = i + k + len / 2;
                const auto tr = re[b] * cr - im[b] * ci;
                const auto ti = re[b] * ci + im[b] * cr;
                re[b] = re[a] - tr;
                im[b] = im[a] - ti;
                re[a] += tr;
                im[a] += ti;
                const auto nr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = nr;
            }
        }
    }
}

// --- envelopes ---------------------------------------------------------------------------------------------------
double tensionCurve (double t, float tension) noexcept
{
    t = clamp01 (t);
    const auto k = std::clamp ((double) tension, -1.0, 1.0);
    if (std::abs (k) < 1.0e-6)
        return t;
    const auto e = 1.0 + std::abs (k) * 4.0;
    return k > 0.0 ? std::pow (t, e) : 1.0 - std::pow (1.0 - t, e);
}

double Envelope::at (double x) const noexcept
{
    if (points.empty())
        return 0.0;
    if (x <= points.front().x)
        return points.front().y;
    if (x >= points.back().x)
        return points.back().y;
    const auto it = std::upper_bound (points.begin(), points.end(), x, [] (double v, const EnvPoint& p) { return v < p.x; });
    const auto& b = *it;
    const auto& a = *(it - 1);
    const auto span = b.x - a.x;
    const auto t = span > 0.0 ? (x - a.x) / span : 1.0;
    return a.y + (b.y - a.y) * tensionCurve (t, b.tension);
}

void Envelope::sort()
{
    std::stable_sort (points.begin(), points.end(), [] (const EnvPoint& a, const EnvPoint& b) { return a.x < b.x; });
}

int Envelope::insert (double x, double y, float tension)
{
    const EnvPoint p { clamp01 (x), clamp01 (y), tension };
    const auto it = std::upper_bound (points.begin(), points.end(), p.x, [] (double v, const EnvPoint& q) { return v < q.x; });
    return (int) (points.insert (it, p) - points.begin());
}

Envelope Envelope::flat (double y)
{
    Envelope e;
    e.points = { { 0.0, y, 0.0f }, { 1.0, y, 0.0f } };
    return e;
}

void scaleLevels (Envelope& env, double centre, double multiply, double offset, double tension)
{
    for (auto& p : env.points)
    {
        auto y = centre + (p.y - centre) * multiply + offset;
        y = clamp01 (y);
        if (std::abs (tension) > 1.0e-6)
            y = tensionCurve (y, (float) -tension); // positive tension lifts the levels (fast start)
        p.y = clamp01 (y);
    }
}

void flipLevels (Envelope& env)
{
    for (auto& p : env.points)
        p.y = 1.0 - p.y;
}

void normalizeLevels (Envelope& env)
{
    if (env.points.empty())
        return;
    const auto [mn, mx] = std::minmax_element (env.points.begin(), env.points.end(), [] (auto& a, auto& b) { return a.y < b.y; });
    const auto lo = mn->y, span = mx->y - mn->y;
    for (auto& p : env.points)
        p.y = span > 1.0e-9 ? (p.y - lo) / span : 1.0;
}

void makePointsSmooth (Envelope& env)
{
    // an S-shape per segment: a point in the middle of each segment with opposite tensions on both halves
    if (env.points.size() < 2)
        return;
    std::vector<EnvPoint> out { env.points.front() };
    for (std::size_t i = 1; i < env.points.size(); ++i)
    {
        const auto& a = env.points[i - 1];
        const auto& b = env.points[i];
        const auto rising = b.y >= a.y;
        out.push_back ({ (a.x + b.x) * 0.5, (a.y + b.y) * 0.5, rising ? 0.5f : -0.5f });
        out.push_back ({ b.x, b.y, rising ? -0.5f : 0.5f });
    }
    env.points = std::move (out);
}

void decimatePoints (Envelope& env, double threshold)
{
    const auto eps = decimationEpsilon (threshold);
    if (eps < 0.0 || env.points.size() < 3)
        return;
    std::vector<bool> keep (env.points.size(), false);
    keep.front() = keep.back() = true;
    rdp (env.points, 0, env.points.size() - 1, eps, keep);
    std::vector<EnvPoint> out;
    for (std::size_t i = 0; i < env.points.size(); ++i)
        if (keep[i])
            out.push_back (env.points[i]);
    env.points = std::move (out);
}

void smoothUp (Envelope& env, double attack, double release, double attackSlope, double releaseSlope, double offset, double decimation)
{
    if (env.points.empty())
        return;
    constexpr int n = 2048;
    auto v = sampleEnvelope (env, n);
    // the largest rise / fall per grid step: a time of 1 = a quarter of the envelope's length for a full swing
    const auto rate = [] (double t) { return 1.0 / std::max (1.0, clamp01 (t) * n * 0.25); };
    const auto up = rate (attack), down = rate (release);
    std::vector<double> out ((std::size_t) n);
    out[0] = v[0];
    for (int i = 1; i < n; ++i)
    {
        const auto prev = out[(std::size_t) i - 1];
        const auto target = v[(std::size_t) i];
        out[(std::size_t) i] = target > prev ? std::min (target, prev + up) : std::max (target, prev - down);
    }
    // offset: move in time
    const auto shift = (int) std::lround (std::clamp (offset, -1.0, 1.0) * n * 0.25);
    if (shift != 0)
    {
        std::vector<double> moved ((std::size_t) n);
        for (int i = 0; i < n; ++i)
            moved[(std::size_t) i] = out[(std::size_t) std::clamp (i - shift, 0, n - 1)];
        out = std::move (moved);
    }
    auto e = fromSamples (out);
    decimatePoints (e, std::max (0.05, decimation));
    // the slopes bend the rising and the falling segments (0.5 = straight)
    for (std::size_t i = 1; i < e.points.size(); ++i)
    {
        const auto rising = e.points[i].y > e.points[i - 1].y;
        e.points[i].tension = (float) ((rising ? attackSlope : releaseSlope) - 0.5) * 2.0f;
    }
    env = std::move (e);
}

void filterEnvelope (Envelope& env, const Envelope& impulse, double width, double offset, double decimation)
{
    if (env.points.empty())
        return;
    constexpr int n = 1024;
    const auto v = sampleEnvelope (env, n);
    const auto half = std::max (1, (int) std::lround (clamp01 (width) * n * 0.25));
    std::vector<double> kernel ((std::size_t) (2 * half + 1));
    double sum = 0.0;
    for (int k = -half; k <= half; ++k)
    {
        const auto x = (double) (k + half) / (double) (2 * half);
        const auto w = impulse.empty() ? 0.5 + 0.5 * std::cos (pi * (double) k / (double) (half + 1)) : std::max (0.0, impulse.at (x));
        kernel[(std::size_t) (k + half)] = w;
        sum += w;
    }
    if (sum <= 1.0e-12)
        return;
    const auto shift = (int) std::lround (std::clamp (offset, -1.0, 1.0) * n * 0.25);
    std::vector<double> out ((std::size_t) n);
    for (int i = 0; i < n; ++i)
    {
        double acc = 0.0;
        for (int k = -half; k <= half; ++k)
            acc += v[(std::size_t) std::clamp (i - shift - k, 0, n - 1)] * kernel[(std::size_t) (k + half)];
        out[(std::size_t) i] = acc / sum;
    }
    auto e = fromSamples (out);
    decimatePoints (e, std::max (0.02, decimation));
    env = std::move (e);
}

Envelope createSequence (const SequenceSettings& s, double beats)
{
    Envelope e;
    const auto steps = std::max (1, (int) std::lround (beats * std::max (1, s.stepsPerUnit)));
    const auto pattern = std::max<std::size_t> (1, s.levels.size());
    const auto stepLength = 1.0 / steps;
    auto level = [&] (int i) {
        auto k = (std::size_t) i % (s.pingPong && pattern > 1 ? 2 * pattern - 2 : pattern);
        if (k >= pattern)
            k = 2 * pattern - 2 - k;
        const auto silent = k < s.breaks.size() && s.breaks[k];
        return silent ? 0.0 : clamp01 (k < s.levels.size() ? s.levels[k] : 1.0);
    };
    e.points.push_back ({ 0.0, 0.0, 0.0f });
    for (int i = 0; i < steps; ++i)
    {
        const auto l = level (i);
        auto start = i * stepLength;
        if (i % 2 == 1)
            start += clamp01 (s.swing) * 0.5 * stepLength;
        const auto gate = std::clamp (s.gate, 0.1, 1.0) * stepLength * (i % 2 == 1 ? 1.0 - clamp01 (s.swing) * 0.5 : 1.0);
        const auto att = std::clamp (s.attack, 0.0, 0.9) * gate;
        const auto dec = std::clamp (s.decay, 0.0, 0.9) * (gate - att);
        const auto end = std::min (1.0, start + gate);
        if (l <= 0.0)
            continue;
        e.points.push_back ({ start, 0.0, 0.0f });
        e.points.push_back ({ start + att, l, 0.0f });
        e.points.push_back ({ start + att + dec, l * clamp01 (s.sustain), -0.4f });
        e.points.push_back ({ end, l * clamp01 (s.sustain), 0.0f });
        e.points.push_back ({ std::min (1.0, end + 1.0e-4), 0.0, 0.0f });
    }
    e.points.push_back ({ 1.0, 0.0, 0.0f });
    e.sort();
    return e;
}

// --- amp envelopes -------------------------------------------------------------------------------------------------
float envelopeGain (double y) noexcept
{
    y = clamp01 (y);
    return (float) (4.0 * y * y); // 0.5 -> 1 (0 dB), 1 -> 4 (+12 dB), 0 -> silence
}

void mixdownEnvelopes (Document& doc, const Envelope& volume, const Envelope& panning, const Envelope& separation)
{
    auto& a = doc.audio;
    const auto n = a.frames();
    if (n == 0)
        return;
    const auto stereo = a.numChannels() > 1;
    for (std::int64_t i = 0; i < n; ++i)
    {
        const auto x = n > 1 ? (double) i / (double) (n - 1) : 0.0;
        const auto g = volume.empty() ? 1.0f : envelopeGain (volume.at (x));
        if (! stereo)
        {
            a.channels[0][(std::size_t) i] *= g;
            continue;
        }
        auto l = a.channels[0][(std::size_t) i];
        auto r = a.channels[1][(std::size_t) i];
        if (! separation.empty())
        {
            const auto sep = (float) (separation.at (x) * 2.0 - 1.0); // -1 merged .. 1 wide
            const auto mid = (l + r) * 0.5f, side = (l - r) * 0.5f * (1.0f + sep);
            l = mid + side;
            r = mid - side;
        }
        if (! panning.empty())
        {
            const auto p = (float) (panning.at (x) * 2.0 - 1.0);
            l *= std::min (1.0f, 1.0f - p);
            r *= std::min (1.0f, 1.0f + p);
        }
        a.channels[0][(std::size_t) i] = l * g;
        a.channels[1][(std::size_t) i] = r * g;
    }
}

void mixdownEnvelopes (Document& doc)
{
    if (! hasEnvelopes (doc))
        return;
    mixdownEnvelopes (doc, doc.volumeEnvelope, doc.panEnvelope, doc.separationEnvelope);
    doc.volumeEnvelope = {};
    doc.panEnvelope = {};
    doc.separationEnvelope = {};
}

bool hasEnvelopes (const Document& doc) noexcept
{
    return ! doc.volumeEnvelope.empty() || ! doc.panEnvelope.empty() || ! doc.separationEnvelope.empty();
}

void remapEnvelopes (Document& doc, std::int64_t oldFrames, Range edited, std::int64_t newLength)
{
    const auto newFrames = doc.audio.frames();
    if (oldFrames <= 1 || newFrames <= 1 || (oldFrames == newFrames && edited.length() == newLength))
        return;
    for (auto* env : { &doc.volumeEnvelope, &doc.panEnvelope, &doc.separationEnvelope })
    {
        if (env->empty())
            continue;
        std::vector<EnvPoint> kept;
        for (auto p : env->points)
        {
            const auto f = p.x * (double) (oldFrames - 1);
            double g;
            if (f <= (double) edited.start)
                g = f;
            else if (f >= (double) edited.end)
                g = f + (double) (newLength - edited.length());
            else if (newLength <= 0)
                continue; // the audio under the point was deleted
            else
                g = (double) edited.start + (f - (double) edited.start) * (double) newLength / (double) std::max<std::int64_t> (1, edited.length());
            p.x = clamp01 (g / (double) (newFrames - 1));
            kept.push_back (p);
        }
        env->points = std::move (kept);
        env->sort();
    }
}

void addPointsAt (Envelope& env, const std::vector<double>& xs)
{
    std::vector<std::pair<double, double>> add;
    for (auto x : xs)
        add.push_back ({ clamp01 (x), env.empty() ? 0.5 : env.at (x) });
    for (auto [x, y] : add)
    {
        const auto exists = std::any_of (env.points.begin(), env.points.end(), [x = x] (const EnvPoint& p) { return std::abs (p.x - x) < 1.0e-9; });
        if (! exists)
            env.insert (x, y);
    }
}

// --- amp ---------------------------------------------------------------------------------------------------------
void ampTool (Document& doc, Range r, const AmpSettings& s)
{
    r = orAll (doc, r);
    auto& a = doc.audio;
    if (r.empty())
        return;
    if (a.numChannels() < 2)
    {
        const auto g = (float) (s.volume * (s.left + s.right) * 0.5);
        for (auto i = r.start; i < r.end; ++i)
            a.channels[0][(std::size_t) i] *= g;
        return;
    }
    const auto p = std::clamp (s.pan, -1.0, 1.0);
    double pl, pr;
    if (s.circularLaw)
    {
        // constant power, normalised so the centre is unity and the far side keeps full level
        const auto theta = (p + 1.0) * pi / 4.0;
        pl = std::min (1.0, std::sqrt (2.0) * std::cos (theta));
        pr = std::min (1.0, std::sqrt (2.0) * std::sin (theta));
    }
    else
    {
        pl = std::min (1.0, 1.0 - p);
        pr = std::min (1.0, 1.0 + p);
    }
    const auto gl = (float) (s.volume * s.left * pl), gr = (float) (s.volume * s.right * pr);
    const auto widen = (float) (1.0 + std::clamp (s.stereoSeparation, -1.0, 1.0));
    for (auto i = r.start; i < r.end; ++i)
    {
        auto l = a.channels[0][(std::size_t) i], rr = a.channels[1][(std::size_t) i];
        if (widen != 1.0f)
        {
            const auto mid = (l + rr) * 0.5f, side = (l - rr) * 0.5f * widen;
            l = mid + side;
            rr = mid - side;
        }
        a.channels[0][(std::size_t) i] = l * gl;
        a.channels[1][(std::size_t) i] = rr * gr;
    }
}

float lossyNormalize (Document& doc, Range r, double maxSeconds)
{
    // FL's way [DEC 0x697920]: try 249 phase rotations of the selection (k * 2pi / 250, the signal turned through its
    // analytic counterpart: x cos t + H(x) sin t), keep the one with the lowest peak, then normalize to full scale.
    // A phase rotation does not change the magnitude spectrum, so the sound stays the same to the ear (mostly) while
    // its peaks get lower, so it can be made louder. Over `maxSeconds` (FL: 30 s) it is a plain normalize.
    r = orAll (doc, r);
    auto& a = doc.audio;
    if (r.empty() || a.channels.empty())
        return 1.0f;
    float before = 0.0f;
    for (const auto& ch : a.channels)
        before = std::max (before, peakOf (ch, r.start, r.end));
    if (before <= 1.0e-9f)
        return 1.0f;
    const auto len = (std::size_t) (r.end - r.start);
    if ((double) len <= maxSeconds * a.sampleRate)
    {
        std::size_t n = 1;
        while (n < len * 2) // padded: the rotation is circular, the padding keeps the ends apart
            n <<= 1;
        std::vector<std::vector<double>> hil;
        for (const auto& ch : a.channels)
        {
            std::vector<double> re (n, 0.0), im (n, 0.0);
            for (std::size_t i = 0; i < len; ++i)
                re[i] = ch[(std::size_t) r.start + i];
            fftInPlace (re, im, false);
            // the Hilbert transform: -j on positive frequencies, +j on negative ones, 0 at DC and Nyquist
            for (std::size_t k = 1; k < n / 2; ++k)
            {
                std::tie (re[k], im[k]) = std::pair (im[k], -re[k]);
                std::tie (re[n - k], im[n - k]) = std::pair (-im[n - k], re[n - k]);
            }
            re[0] = im[0] = re[n / 2] = im[n / 2] = 0.0;
            fftInPlace (re, im, true);
            for (auto& v : re)
                v /= (double) n;
            re.resize (len);
            hil.push_back (std::move (re));
        }
        double bestPeak = before, bestAngle = 0.0;
        for (int k = 1; k < 250; ++k)
        {
            const auto t = (double) k * 2.0 * pi / 250.0, c = std::cos (t), s = std::sin (t);
            double pk = 0.0;
            for (std::size_t chn = 0; chn < a.channels.size() && pk < bestPeak; ++chn)
                for (std::size_t i = 0; i < len; ++i)
                    pk = std::max (pk, std::abs (a.channels[chn][(std::size_t) r.start + i] * c + hil[chn][i] * s));
            if (pk < bestPeak)
            {
                bestPeak = pk;
                bestAngle = t;
            }
        }
        if (bestAngle != 0.0)
        {
            const auto c = std::cos (bestAngle), s = std::sin (bestAngle);
            for (std::size_t chn = 0; chn < a.channels.size(); ++chn)
                for (std::size_t i = 0; i < len; ++i)
                {
                    auto& v = a.channels[chn][(std::size_t) r.start + i];
                    v = (float) (v * c + hil[chn][i] * s);
                }
        }
    }
    float after = 0.0f;
    for (const auto& ch : a.channels)
        after = std::max (after, peakOf (ch, r.start, r.end));
    if (after <= 1.0e-9f)
        return 1.0f;
    const auto gain = 1.0f / after;
    for (auto& ch : a.channels)
        for (auto i = r.start; i < r.end; ++i)
            ch[(std::size_t) i] = std::clamp (ch[(std::size_t) i] * gain, -1.0f, 1.0f);
    return before / after; // how much lower the peak got (FL shows it in dB after the edit)
}

bool monoIfMono (Document& doc, float tolerance)
{
    auto& a = doc.audio;
    if (a.numChannels() < 2)
        return false;
    for (std::int64_t i = 0; i < a.frames(); ++i)
        if (std::abs (a.channels[0][(std::size_t) i] - a.channels[1][(std::size_t) i]) > tolerance)
            return false;
    a.channels.resize (1);
    return true;
}

void generateNoise (Document& doc, Range r, float level, std::uint32_t seed, std::int64_t framesIfEmpty)
{
    Rng rng (seed);
    if (doc.audio.channels.empty())
    {
        doc.audio = silence (2, doc.audio.sampleRate > 0.0 ? doc.audio.sampleRate : 44100.0, 0);
    }
    r = clamp (doc, r);
    if (r.empty())
    {
        if (framesIfEmpty <= 0)
            return;
        insertSilence (doc, r.start, framesIfEmpty);
        r = { r.start, r.start + framesIfEmpty };
    }
    for (auto& ch : doc.audio.channels)
        for (auto i = r.start; i < r.end; ++i)
            ch[(std::size_t) i] = (float) rng.bipolar() * level;
}

// --- noise gating --------------------------------------------------------------------------------------------------
float acquireNoiseThreshold (const Audio& audio, Range r) noexcept
{
    float p = 0.0f;
    r.start = std::clamp<std::int64_t> (r.start, 0, audio.frames());
    r.end = std::clamp<std::int64_t> (r.end, r.start, audio.frames());
    if (r.empty())
        r = { 0, audio.frames() };
    for (const auto& ch : audio.channels)
        p = std::max (p, peakOf (ch, r.start, r.end));
    return p;
}

std::vector<Range> quietParts (const Audio& audio, Range r, float threshold, double minSilence)
{
    std::vector<Range> parts;
    if (audio.empty())
        return parts;
    r.start = std::clamp<std::int64_t> (r.start, 0, audio.frames());
    r.end = std::clamp<std::int64_t> (r.end, r.start, audio.frames());
    const auto window = std::max<std::int64_t> (16, (std::int64_t) (audio.sampleRate * 0.005)); // 5 ms
    const auto minFrames = (std::int64_t) (minSilence * audio.sampleRate);
    std::int64_t runStart = -1;
    for (auto w = r.start; w < r.end; w += window)
    {
        const auto e = std::min (r.end, w + window);
        float p = 0.0f;
        for (const auto& ch : audio.channels)
            p = std::max (p, peakOf (ch, w, e));
        if (p <= threshold)
        {
            if (runStart < 0)
                runStart = w;
        }
        else if (runStart >= 0)
        {
            if (w - runStart >= minFrames)
                parts.push_back ({ runStart, w });
            runStart = -1;
        }
    }
    if (runStart >= 0 && r.end - runStart >= minFrames)
        parts.push_back ({ runStart, r.end });
    return parts;
}

int gateNoise (Document& doc, Range r, float threshold)
{
    r = orAll (doc, r);
    const auto parts = quietParts (doc.audio, r, threshold);
    const auto fade = (std::int64_t) (doc.audio.sampleRate * 0.003);
    for (const auto& p : parts)
    {
        // fade out into the quiet part, silence, fade back in
        const auto a = p.start, b = p.end;
        const auto fa = std::min (fade, (b - a) / 3), fb = fa;
        fadeFrames (doc.audio, a, a + fa, false);
        for (auto& ch : doc.audio.channels)
            std::fill (ch.begin() + (std::ptrdiff_t) (a + fa), ch.begin() + (std::ptrdiff_t) (b - fb), 0.0f);
        fadeFrames (doc.audio, b - fb, b, true);
    }
    return (int) parts.size();
}

void trimSideNoise (Document& doc, Range r, float threshold)
{
    r = orAll (doc, r);
    if (r.empty())
        return;
    const auto parts = quietParts (doc.audio, r, threshold, 0.0);
    if (! parts.empty() && parts.back().end == r.end)
        erase (doc, parts.back());
    if (! parts.empty() && parts.front().start == r.start && ! (parts.size() == 1 && parts.front().end == r.end))
        erase (doc, parts.front());
}

int trimNoiseAndSlice (Document& doc, Range r, float threshold)
{
    r = orAll (doc, r);
    if (r.empty())
        return 0;
    const auto pad = (std::int64_t) (doc.audio.sampleRate * 0.005);
    auto parts = quietParts (doc.audio, r, threshold, 0.05);
    // keep a little of each quiet part next to the sound (the attack's lead-in and the release)
    std::vector<Range> cuts;
    for (auto p : parts)
    {
        if (p.start > r.start)
            p.start += pad;
        if (p.end < r.end)
            p.end -= pad;
        if (p.end > p.start)
            cuts.push_back (p);
    }
    // the remaining pieces, at their positions after the cuts (each piece moves left by the cuts before it)
    std::vector<Range> moved;
    std::int64_t pos = r.start, shift = 0;
    for (const auto& c : cuts)
    {
        if (c.start > pos)
            moved.push_back ({ pos - shift, c.start - shift });
        shift += c.end - c.start;
        pos = c.end;
    }
    if (pos < r.end)
        moved.push_back ({ pos - shift, r.end - shift });
    for (auto it = cuts.rbegin(); it != cuts.rend(); ++it) // from the end so earlier positions stay valid
        erase (doc, *it);
    int n = 0;
    for (const auto& m : moved)
    {
        fadeFrames (doc.audio, m.start, std::min (m.end, m.start + pad / 2), true);
        fadeFrames (doc.audio, std::max (m.start, m.end - pad), m.end, false);
        addRegion (doc, m, "Slice " + std::to_string (++n));
    }
    return n;
}

// --- regions -------------------------------------------------------------------------------------------------------
int normalizeRegions (Document& doc)
{
    int n = 0;
    const auto markers = doc.markers; // (the edits below keep them, but iterate over a copy)
    for (const auto& m : markers)
        if (m.isRegion())
        {
            normalize (doc, { m.start, m.end }, 0.0f);
            ++n;
        }
    return n;
}

int declickRegions (Document& doc, bool in, bool out)
{
    int n = 0;
    const auto markers = doc.markers; // (the edits below keep them, but iterate over a copy)
    for (const auto& m : markers)
        if (m.isRegion())
        {
            if (in)
                declickIn (doc, { m.start, m.end });
            if (out)
                declickOut (doc, { m.start, m.end });
            ++n;
        }
    return n;
}

int perfectRegions (Document& doc)
{
    const auto radius = (std::int64_t) (doc.audio.sampleRate * 0.01);
    for (auto& m : doc.markers)
        if (m.isRegion())
        {
            const auto s = nearestZeroCrossing (doc.audio, m.start, radius);
            const auto e = nearestZeroCrossing (doc.audio, m.end, radius);
            if (e > s)
            {
                m.start = s;
                m.end = e;
            }
        }
    sortMarkers (doc);
    return declickRegions (doc, true, true);
}

// --- spectral --------------------------------------------------------------------------------------------------
double curveFrequency (double x, double freqScale, double sampleRate) noexcept
{
    const auto nyquist = sampleRate * 0.5;
    x = clamp01 (x);
    const auto lin = x * nyquist;
    const auto lo = 20.0;
    const auto log = lo * std::pow (nyquist / lo, x) - lo * (1.0 - x); // reaches 0 at x = 0
    const auto s = clamp01 (freqScale);
    return std::max (0.0, lin * (1.0 - s) + log * s);
}

double curvePosition (double hz, double freqScale, double sampleRate) noexcept
{
    double lo = 0.0, hi = 1.0;
    for (int i = 0; i < 48; ++i)
    {
        const auto mid = 0.5 * (lo + hi);
        (curveFrequency (mid, freqScale, sampleRate) < hz ? lo : hi) = mid;
    }
    return 0.5 * (lo + hi);
}

double eqGain (double y) noexcept
{
    y = clamp01 (y);
    if (y >= 0.5)
        return dbToGain ((y - 0.5) * 2.0 * 18.0);
    return 4.0 * y * y; // 0.25 -> -6 dB... 0 -> silence
}

namespace
{
// per-bin gains of an EQ curve for an FFT of `size`
std::vector<double> curveGains (const Envelope& curve, double freqScale, double sampleRate, int size, bool smooth)
{
    const auto bins = size / 2 + 1;
    std::vector<double> g ((std::size_t) bins);
    for (int b = 0; b < bins; ++b)
    {
        const auto hz = (double) b * sampleRate / size;
        g[(std::size_t) b] = eqGain (curve.empty() ? 0.5 : curve.at (curvePosition (hz, freqScale, sampleRate)));
    }
    if (smooth)
    {
        // a 1/6-octave running average in dB (fewer sharp steps -> less ringing)
        std::vector<double> db ((std::size_t) bins), out ((std::size_t) bins);
        for (int b = 0; b < bins; ++b)
            db[(std::size_t) b] = 20.0 * std::log10 (std::max (1.0e-6, g[(std::size_t) b]));
        for (int b = 0; b < bins; ++b)
        {
            const auto w = std::max (1, (int) std::lround (b * (std::pow (2.0, 1.0 / 12.0) - 1.0)));
            double acc = 0.0;
            int n = 0;
            for (int k = std::max (0, b - w); k <= std::min (bins - 1, b + w); ++k, ++n)
                acc += db[(std::size_t) k];
            out[(std::size_t) b] = g[(std::size_t) b] <= 1.0e-6 ? 0.0 : dbToGain (acc / n);
        }
        g = std::move (out);
    }
    return g;
}

void applyGains (Document& doc, Range r, const std::vector<double>& gains, int size, double mix, Progress* progress)
{
    // a little context on both sides so the edges are processed like the middle
    const auto from = r.start - size, to = r.end + size;
    auto parts = excerpt (doc.audio, from, to);
    const auto channels = (int) parts.size();
    for (int c = 0; c < channels; ++c)
    {
        auto out = stftProcess (parts[(std::size_t) c], size, [&] (std::vector<double>& re, std::vector<double>& im, std::int64_t) {
            for (int b = 0; b <= size / 2; ++b)
            {
                re[(std::size_t) b] *= gains[(std::size_t) b];
                im[(std::size_t) b] *= gains[(std::size_t) b];
            }
        }, progress, (double) c / channels, 1.0 / channels);
        if (stop (progress))
            return;
        auto& ch = doc.audio.channels[(std::size_t) c];
        for (auto i = r.start; i < r.end; ++i)
        {
            const auto dry = ch[(std::size_t) i];
            ch[(std::size_t) i] = (float) (dry * (1.0 - mix) + out[(std::size_t) (i - from)] * mix);
        }
    }
}
} // namespace

void equalize (Document& doc, Range r, const EqSettings& s, Progress* progress)
{
    r = orAll (doc, r);
    if (r.empty())
        return;
    constexpr int size = 8192;
    const auto gains = curveGains (s.gain, s.freqScale, doc.audio.sampleRate, size, s.smooth);
    applyGains (doc, r, gains, size, clamp01 (s.mix), progress);
    report (progress, 1.0);
}

Envelope unharmonicsCurve (double fundamentalHz, int width, double freqScale, double sampleRate)
{
    Envelope e;
    if (fundamentalHz <= 0.0)
        return Envelope::flat (0.5);
    const double widths[] = { 0.06, 0.12, 0.25 }; // of the harmonic spacing
    const auto w = widths[std::clamp (width, 0, 2)];
    const auto nyquist = sampleRate * 0.5;
    e.points.push_back ({ 0.0, 0.0, 0.0f });
    for (auto h = fundamentalHz; h < nyquist; h += fundamentalHz)
    {
        const auto a = curvePosition (h - fundamentalHz * w, freqScale, sampleRate);
        const auto m = curvePosition (h, freqScale, sampleRate);
        const auto b = curvePosition (h + fundamentalHz * w, freqScale, sampleRate);
        e.points.push_back ({ a, 0.0, 0.0f });
        e.points.push_back ({ m, 0.5, 0.0f });
        e.points.push_back ({ b, 0.0, 0.0f });
    }
    e.points.push_back ({ 1.0, 0.0, 0.0f });
    e.sort();
    return e;
}

// --- blur ------------------------------------------------------------------------------------------------------------
// Each bin's magnitude is smeared over time with the impulse shape (amount = its length, offset = where it sits),
// with phases that keep advancing at the bin's own rate plus a little randomness, so the smear sounds like a
// diffuse, frozen continuation of the sound instead of a comb.
void blur (Document& doc, Range r, const BlurSettings& s, Progress* progress)
{
    r = orAll (doc, r);
    if (r.empty())
        return;
    constexpr int size = 2048, hop = size / 4;
    const auto sr = doc.audio.sampleRate;
    const auto lengthSec = 0.02 + 1.98 * clamp01 (s.amount);
    const auto taps = std::max (1, (int) std::lround (lengthSec * sr / hop));
    const auto shift = (int) std::lround (std::clamp (s.offset, -1.0, 1.0) * taps);
    std::vector<double> kernel ((std::size_t) taps);
    double sum = 0.0;
    // the impulse spans the blur's length centred on the sound (x 0.5 = the sound's own moment, like FL's maps)
    for (int k = 0; k < taps; ++k)
    {
        const auto x = taps > 1 ? (double) k / (taps - 1) : 0.5;
        const auto w = s.impulse.empty() ? 0.5 - 0.5 * std::cos (2.0 * pi * x) : std::max (0.0, s.impulse.at (x));
        kernel[(std::size_t) k] = w;
        sum += w;
    }
    if (sum <= 1.0e-12)
        return;
    for (auto& k : kernel)
        k /= sum;

    const auto context = (std::int64_t) taps * hop + size;
    const auto from = s.useOutside ? r.start - context : r.start;
    const auto to = s.useOutside ? r.end + context : r.end;
    auto parts = excerpt (doc.audio, from, to);
    const auto channels = (int) parts.size();
    const auto bins = size / 2 + 1;
    Rng rng (12345);

    for (int c = 0; c < channels; ++c)
    {
        // pass 1: magnitudes per frame
        std::vector<std::vector<float>> mags;
        stftProcess (parts[(std::size_t) c], size, [&] (std::vector<double>& re, std::vector<double>& im, std::int64_t) {
            std::vector<float> m ((std::size_t) bins);
            for (int b = 0; b < bins; ++b)
                m[(std::size_t) b] = (float) std::hypot (re[(std::size_t) b], im[(std::size_t) b]);
            mags.push_back (std::move (m));
        }, progress, (double) c / channels, 0.3 / channels);
        if (stop (progress))
            return;
        const auto frames = (std::int64_t) mags.size();
        std::vector<double> phase ((std::size_t) bins, 0.0);
        // pass 2: the smeared magnitudes with running phases
        auto out = stftProcess (parts[(std::size_t) c], size, [&] (std::vector<double>& re, std::vector<double>& im, std::int64_t f) {
            for (int b = 0; b < bins; ++b)
            {
                double m = 0.0;
                for (int k = 0; k < taps; ++k)
                {
                    const auto src = f - (k - taps / 2) - shift;
                    if (src >= 0 && src < frames)
                        m += mags[(std::size_t) src][(std::size_t) b] * kernel[(std::size_t) k];
                }
                phase[(std::size_t) b] += 2.0 * pi * b * hop / size + rng.bipolar() * 0.6;
                re[(std::size_t) b] = m * std::cos (phase[(std::size_t) b]);
                im[(std::size_t) b] = m * std::sin (phase[(std::size_t) b]);
            }
        }, progress, (double) c / channels + 0.3 / channels, 0.7 / channels);
        if (stop (progress))
            return;
        auto& ch = doc.audio.channels[(std::size_t) c];
        const auto mix = clamp01 (s.mix);
        for (auto i = r.start; i < r.end; ++i)
            ch[(std::size_t) i] = (float) (ch[(std::size_t) i] * (1.0 - mix) + out[(std::size_t) (i - from)] * mix);
    }
    report (progress, 1.0);
}

// --- convolution reverb ------------------------------------------------------------------------------------------
Audio makeReverbImpulse (const Envelope& shape, double seconds, double sampleRate, std::uint32_t seed)
{
    Audio ir;
    ir.sampleRate = sampleRate;
    const auto n = std::max<std::int64_t> (16, (std::int64_t) (std::clamp (seconds, 0.01, 30.0) * sampleRate));
    ir.channels.assign (2, std::vector<float> ((std::size_t) n));
    for (int c = 0; c < 2; ++c)
    {
        Rng rng (seed + (std::uint32_t) c * 7919u);
        double lp = 0.0;
        for (std::int64_t i = 0; i < n; ++i)
        {
            const auto x = (double) i / (double) (n - 1);
            const auto level = shape.empty() ? std::exp (-6.9 * x) : std::max (0.0, shape.at (x));
            // the tail darkens: a one-pole low-pass whose cut-off falls from ~16 kHz to ~2 kHz
            const auto cutoff = 16000.0 * std::pow (0.125, x);
            const auto a = std::exp (-2.0 * pi * cutoff / sampleRate);
            lp = lp * a + rng.bipolar() * (1.0 - a);
            ir.channels[(std::size_t) c][(std::size_t) i] = (float) (lp * level);
        }
    }
    // unit energy per channel: a wet of 1 keeps the loudness of a broadband input
    for (auto& ch : ir.channels)
    {
        double e = 0.0;
        for (auto v : ch)
            e += (double) v * v;
        const auto g = e > 0.0 ? 1.0 / std::sqrt (e) : 0.0;
        for (auto& v : ch)
            v = (float) (v * g);
    }
    return ir;
}

namespace
{
// linear convolution of x with h by FFT blocks (overlap-add); returns x.size() + h.size() - 1 samples
std::vector<double> convolve (const std::vector<float>& x, const std::vector<float>& h, Progress* progress, double from, double span)
{
    const auto nx = x.size(), nh = h.size();
    std::vector<double> y (nx + nh - 1, 0.0);
    if (nx == 0 || nh == 0)
        return y;
    std::size_t size = 1;
    while (size < 2 * nh)
        size <<= 1;
    size = std::max<std::size_t> (size, 4096);
    const auto block = size - nh + 1;
    std::vector<double> hr (size, 0.0), hi (size, 0.0);
    for (std::size_t i = 0; i < nh; ++i)
        hr[i] = h[i];
    fftInPlace (hr, hi, false);
    std::vector<double> re (size), im (size);
    const auto blocks = (nx + block - 1) / block;
    for (std::size_t b = 0; b < blocks; ++b)
    {
        if (stop (progress))
            return y;
        report (progress, from + span * (double) b / (double) blocks);
        std::fill (re.begin(), re.end(), 0.0);
        std::fill (im.begin(), im.end(), 0.0);
        const auto start = b * block;
        for (std::size_t i = 0; i < block && start + i < nx; ++i)
            re[i] = x[start + i];
        fftInPlace (re, im, false);
        for (std::size_t k = 0; k < size; ++k)
        {
            const auto r = re[k] * hr[k] - im[k] * hi[k];
            im[k] = re[k] * hi[k] + im[k] * hr[k];
            re[k] = r;
        }
        fftInPlace (re, im, true);
        for (std::size_t i = 0; i < size && start + i < y.size(); ++i)
            y[start + i] += re[i] / (double) size;
    }
    return y;
}
} // namespace

void convolutionReverb (Document& doc, Range r, const ReverbSettings& s, Progress* progress)
{
    r = orAll (doc, r);
    if (r.empty())
        return;
    const auto sr = doc.audio.sampleRate;
    auto ir = s.impulse.empty() ? makeReverbImpulse (s.shape, s.length, sr) : conform (s.impulse, 2, sr);
    if (ir.empty())
        return;
    const auto irLen = ir.frames();
    const auto delay = (std::int64_t) std::llround (std::clamp (s.offset, -1.0, 1.0) * sr); // pre-delay (negative: earlier)
    // the input: the range, plus (use outside) the audio before it whose tail rings into the range
    const auto from = s.useOutside ? std::max<std::int64_t> (0, r.start - irLen) : r.start;
    const auto inputs = excerpt (doc.audio, from, r.end);
    const auto tail = s.addTail ? irLen + std::max<std::int64_t> (0, delay) : 0;
    if (tail > 0)
        insertSilence (doc, r.end, tail);
    const auto channels = doc.audio.numChannels();
    const auto dry = clamp01 (s.dry), wet = clamp01 (s.wet);
    for (int c = 0; c < channels; ++c)
    {
        const auto& h = ir.channels[(std::size_t) std::min (c, ir.numChannels() - 1)];
        // a mono document hears the average of both impulse channels
        std::vector<float> hm;
        if (channels == 1 && ir.numChannels() > 1)
        {
            hm.resize (h.size());
            for (std::size_t i = 0; i < h.size(); ++i)
                hm[i] = 0.5f * (ir.channels[0][i] + ir.channels[1][i]);
        }
        const auto y = convolve (inputs[(std::size_t) c], hm.empty() ? h : hm, progress, (double) c / channels, 1.0 / channels);
        if (stop (progress))
            return;
        auto& ch = doc.audio.channels[(std::size_t) c];
        const auto end = r.end + tail;
        for (auto i = r.start; i < end; ++i)
        {
            const auto k = i - from - delay;
            const auto w = k >= 0 && k < (std::int64_t) y.size() ? y[(std::size_t) k] : 0.0;
            const auto d = i < r.end ? (double) ch[(std::size_t) i] : 0.0;
            ch[(std::size_t) i] = (float) (d * dry + w * wet);
        }
    }
    report (progress, 1.0);
}

// --- denoise ---------------------------------------------------------------------------------------------------------
NoiseProfile acquireNoiseProfile (const Audio& audio, Range r, int fftSize)
{
    NoiseProfile p;
    r.start = std::clamp<std::int64_t> (r.start, 0, audio.frames());
    r.end = std::clamp<std::int64_t> (r.end, r.start, audio.frames());
    if (r.empty())
        r = { 0, audio.frames() };
    if (r.length() < 64 || audio.channels.empty())
        return p;
    const auto bins = fftSize / 2 + 1;
    p.fftSize = fftSize;
    p.sampleRate = audio.sampleRate;
    p.magnitude.assign ((std::size_t) bins, 0.0f);
    const auto window = sqrtHann (fftSize);
    std::vector<double> re ((std::size_t) fftSize), im ((std::size_t) fftSize), sum ((std::size_t) bins, 0.0);
    int frames = 0;
    const auto hop = fftSize / 4;
    for (auto start = r.start; start == r.start || start + fftSize <= r.end; start += hop)
    {
        for (const auto& ch : audio.channels)
        {
            for (int i = 0; i < fftSize; ++i)
            {
                const auto k = start + i;
                re[(std::size_t) i] = (k < r.end ? (double) ch[(std::size_t) k] : 0.0) * window[(std::size_t) i];
                im[(std::size_t) i] = 0.0;
            }
            fftInPlace (re, im, false);
            for (int b = 0; b < bins; ++b)
                sum[(std::size_t) b] += std::hypot (re[(std::size_t) b], im[(std::size_t) b]);
            ++frames;
        }
        if (start + fftSize > r.end)
            break;
    }
    for (int b = 0; b < bins; ++b)
        p.magnitude[(std::size_t) b] = (float) (sum[(std::size_t) b] / std::max (1, frames));
    return p;
}

namespace
{
// a noise floor from the quietest frames when no profile was taken: per bin, the 10th percentile over time
NoiseProfile estimateNoiseFloor (const Audio& audio, Range r, int fftSize)
{
    NoiseProfile p;
    const auto bins = fftSize / 2 + 1;
    const auto window = sqrtHann (fftSize);
    std::vector<std::vector<float>> perBin ((std::size_t) bins);
    std::vector<double> re ((std::size_t) fftSize), im ((std::size_t) fftSize);
    const auto hop = fftSize / 2;
    for (auto start = r.start; start + fftSize <= r.end || start == r.start; start += hop)
    {
        for (const auto& ch : audio.channels)
        {
            for (int i = 0; i < fftSize; ++i)
            {
                const auto k = start + i;
                re[(std::size_t) i] = (k < r.end ? (double) ch[(std::size_t) k] : 0.0) * window[(std::size_t) i];
                im[(std::size_t) i] = 0.0;
            }
            fftInPlace (re, im, false);
            for (int b = 0; b < bins; ++b)
                perBin[(std::size_t) b].push_back ((float) std::hypot (re[(std::size_t) b], im[(std::size_t) b]));
        }
        if (start + fftSize > r.end)
            break;
    }
    p.fftSize = fftSize;
    p.sampleRate = audio.sampleRate;
    p.magnitude.resize ((std::size_t) bins);
    for (int b = 0; b < bins; ++b)
    {
        auto& v = perBin[(std::size_t) b];
        if (v.empty())
            continue;
        const auto k = v.size() / 10;
        std::nth_element (v.begin(), v.begin() + (std::ptrdiff_t) k, v.end());
        p.magnitude[(std::size_t) b] = v[k];
    }
    return p;
}

// interpolating repair of [a, b) from the samples around it (a cubic through two points on each side)
void repairSpan (std::vector<float>& x, std::int64_t a, std::int64_t b)
{
    const auto n = (std::int64_t) x.size();
    const auto len = b - a;
    if (len <= 0 || a < 2 || b + 1 >= n)
        return;
    // Hermite between x[a-1] and x[b] with slopes from their outer neighbours (per sample)
    const double p0 = x[(std::size_t) (a - 1)], p1 = x[(std::size_t) b];
    const double m0 = (x[(std::size_t) (a - 1)] - x[(std::size_t) (a - 2)]) * (double) (len + 1);
    const double m1 = (x[(std::size_t) (b + 1)] - x[(std::size_t) b]) * (double) (len + 1);
    for (auto i = a; i < b; ++i)
    {
        const auto t = (double) (i - a + 1) / (double) (len + 1);
        const auto t2 = t * t, t3 = t2 * t;
        x[(std::size_t) i] = (float) ((2 * t3 - 3 * t2 + 1) * p0 + (t3 - 2 * t2 + t) * m0 + (-2 * t3 + 3 * t2) * p1 + (t3 - t2) * m1);
    }
}
} // namespace

int declip (Audio& audio, Range r, double threshold)
{
    int repaired = 0;
    r.start = std::clamp<std::int64_t> (r.start, 0, audio.frames());
    r.end = std::clamp<std::int64_t> (r.end, r.start, audio.frames());
    for (auto& ch : audio.channels)
    {
        const auto peak = peakOf (ch, r.start, r.end);
        if (peak <= 0.0f)
            continue;
        const auto limit = (float) std::clamp (threshold, 0.05, 1.0) * peak;
        for (auto i = r.start; i < r.end;)
        {
            if (std::abs (ch[(std::size_t) i]) < limit)
            {
                ++i;
                continue;
            }
            auto j = i;
            const auto sign = ch[(std::size_t) i] > 0.0f;
            while (j < r.end && std::abs (ch[(std::size_t) j]) >= limit && (ch[(std::size_t) j] > 0.0f) == sign)
                ++j;
            if (j - i >= 2 && j - i < 2000) // a flat run: a clipped peak
            {
                std::vector<float> before (ch.begin() + (std::ptrdiff_t) i, ch.begin() + (std::ptrdiff_t) j);
                repairSpan (ch, i, j);
                // the rebuilt peak must keep going past the clip level (never fold back inside it)
                for (auto k = i; k < j; ++k)
                    if (std::abs (ch[(std::size_t) k]) < std::abs (before[(std::size_t) (k - i)]) || (ch[(std::size_t) k] > 0.0f) != sign)
                        ch[(std::size_t) k] = before[(std::size_t) (k - i)];
                ++repaired;
            }
            i = j;
        }
    }
    return repaired;
}

int declick (Audio& audio, Range r, double sensitivity)
{
    int fixed = 0;
    r.start = std::clamp<std::int64_t> (r.start, 0, audio.frames());
    r.end = std::clamp<std::int64_t> (r.end, r.start, audio.frames());
    const auto factor = 24.0 - 18.0 * clamp01 (sensitivity); // how far over the local residual a click must stick out
    const auto window = (std::int64_t) 256;
    const auto maxClick = std::max<std::int64_t> (4, (std::int64_t) (audio.sampleRate * 0.0015));
    for (auto& ch : audio.channels)
    {
        const auto n = r.end - r.start;
        if (n < 8)
            continue;
        // second difference = what a smooth signal does not explain
        std::vector<double> e ((std::size_t) n, 0.0);
        for (auto i = r.start + 1; i + 1 < r.end; ++i)
            e[(std::size_t) (i - r.start)] = std::abs ((double) ch[(std::size_t) (i - 1)] - 2.0 * ch[(std::size_t) i] + ch[(std::size_t) (i + 1)]);
        // running mean of |e| over the window
        std::vector<double> prefix ((std::size_t) n + 1, 0.0);
        for (std::int64_t i = 0; i < n; ++i)
            prefix[(std::size_t) i + 1] = prefix[(std::size_t) i] + e[(std::size_t) i];
        for (std::int64_t i = 2; i + 2 < n;)
        {
            const auto lo = std::max<std::int64_t> (0, i - window), hi = std::min (n, i + window);
            const auto mean = (prefix[(std::size_t) hi] - prefix[(std::size_t) lo]) / (double) (hi - lo);
            if (mean > 1.0e-7 && e[(std::size_t) i] > factor * mean)
            {
                auto j = i;
                while (j < n && j - i < maxClick && e[(std::size_t) j] > factor * 0.3 * mean)
                    ++j;
                if (j - i < maxClick)
                {
                    repairSpan (ch, r.start + i - 1, r.start + j + 1);
                    ++fixed;
                }
                i = j + 2;
                continue;
            }
            ++i;
        }
    }
    return fixed;
}

void denoise (Document& doc, Range r, const NoiseProfile& profile, const DenoiseSettings& s, Progress* progress)
{
    r = orAll (doc, r);
    if (r.empty())
        return;
    if (s.declip)
        declip (doc.audio, r, s.clipThreshold);
    if (s.declick)
        declick (doc.audio, r, s.clickSensitivity);
    if (! s.denoise && ! s.noiseOnly)
        return;
    constexpr int size = 4096, hop = size / 4;
    const auto sr = doc.audio.sampleRate;
    const auto noise = profile.valid() && profile.fftSize == size ? profile : estimateNoiseFloor (doc.audio, r, size);
    if (! noise.valid())
        return;
    const auto bins = size / 2 + 1;
    // the floor per bin: the profile, scaled by the threshold (+-18 dB) and the threshold curve
    std::vector<double> floor ((std::size_t) bins);
    for (int b = 0; b < bins; ++b)
    {
        const auto hz = (double) b * sr / size;
        const auto curve = s.thresholdCurve.empty() ? 0.5 : s.thresholdCurve.at (curvePosition (hz, s.freqScale, sr));
        floor[(std::size_t) b] = noise.magnitude[(std::size_t) b] * dbToGain ((clamp01 (s.threshold) - 0.5) * 36.0 + (curve - 0.5) * 36.0);
    }
    const auto minGain = clamp01 (s.amount) >= 0.999 ? 0.0 : dbToGain (-clamp01 (s.amount) * 36.0);
    // gain smoothing over time (fast open, slower close) and across neighbouring bins: no "musical noise"
    const auto openCoef = 0.2, closeCoef = std::exp (-(double) hop / (0.08 * sr));
    const auto from = r.start - size, to = r.end + size;
    auto parts = excerpt (doc.audio, from, to);
    const auto channels = (int) parts.size();
    for (int c = 0; c < channels; ++c)
    {
        std::vector<double> held ((std::size_t) bins, 1.0), g ((std::size_t) bins);
        auto out = stftProcess (parts[(std::size_t) c], size, [&] (std::vector<double>& re, std::vector<double>& im, std::int64_t) {
            for (int b = 0; b < bins; ++b)
            {
                const auto mag = std::hypot (re[(std::size_t) b], im[(std::size_t) b]);
                const auto ratio = floor[(std::size_t) b] / std::max (1.0e-12, mag);
                g[(std::size_t) b] = std::max (minGain, std::sqrt (std::max (0.0, 1.0 - ratio * ratio)));
            }
            for (int b = 0; b < bins; ++b)
            {
                const auto spread = std::max ({ g[(std::size_t) b], b > 0 ? g[(std::size_t) b - 1] * 0.85 : 0.0, b + 1 < bins ? g[(std::size_t) b + 1] * 0.85 : 0.0 });
                auto& h = held[(std::size_t) b];
                h = spread > h ? h + (spread - h) * (1.0 - openCoef) : spread + (h - spread) * closeCoef;
                const auto gain = s.noiseOnly ? 1.0 - h : h;
                re[(std::size_t) b] *= gain;
                im[(std::size_t) b] *= gain;
            }
        }, progress, (double) c / channels, 1.0 / channels);
        if (stop (progress))
            return;
        auto& ch = doc.audio.channels[(std::size_t) c];
        for (auto i = r.start; i < r.end; ++i)
            ch[(std::size_t) i] = out[(std::size_t) (i - from)];
    }
    report (progress, 1.0);
}

// --- vocal denoise ----------------------------------------------------------------------------------------------------
// A speech-oriented noise suppressor: the noise floor per bin is tracked by minimum statistics (the lowest
// smoothed power over the last ~1.5 s, corrected upwards), the speech gain is a decision-directed Wiener gain
// (Ephraim-Malah style a-priori SNR), with a floor so the result never pumps to silence. mix > 0 blends towards the
// voice, mix < 0 towards the removed noise.
void vocalDenoise (Document& doc, Range r, double mix, Progress* progress)
{
    r = orAll (doc, r);
    if (r.empty())
        return;
    constexpr int size = 2048, hop = size / 4;
    const auto sr = doc.audio.sampleRate;
    const auto bins = size / 2 + 1;
    const auto from = r.start - size, to = r.end + size;
    auto parts = excerpt (doc.audio, from, to);
    const auto channels = (int) parts.size();
    const auto history = std::max (8, (int) std::lround (1.5 * sr / hop));
    const auto smoothCoef = 0.7;
    mix = std::clamp (mix, -1.0, 1.0);
    for (int c = 0; c < channels; ++c)
    {
        std::vector<double> smoothed ((std::size_t) bins, 0.0), noise ((std::size_t) bins, 0.0), prevClean ((std::size_t) bins, 0.0);
        std::vector<std::vector<double>> minWindow ((std::size_t) bins);
        // sub-windows of the minimum tracker (8 of history/8 frames each)
        const auto sub = std::max (1, history / 8);
        std::vector<double> subMin ((std::size_t) bins, 1.0e30);
        std::int64_t frame = 0;
        auto out = stftProcess (parts[(std::size_t) c], size, [&] (std::vector<double>& re, std::vector<double>& im, std::int64_t) {
            for (int b = 0; b < bins; ++b)
            {
                const auto power = re[(std::size_t) b] * re[(std::size_t) b] + im[(std::size_t) b] * im[(std::size_t) b];
                auto& sm = smoothed[(std::size_t) b];
                sm = frame == 0 ? power : smoothCoef * sm + (1.0 - smoothCoef) * power;
                subMin[(std::size_t) b] = std::min (subMin[(std::size_t) b], sm);
                auto& w = minWindow[(std::size_t) b];
                if (frame % sub == sub - 1)
                {
                    w.push_back (subMin[(std::size_t) b]);
                    if (w.size() > 8)
                        w.erase (w.begin());
                    subMin[(std::size_t) b] = 1.0e30;
                }
                auto m = subMin[(std::size_t) b];
                for (auto v : w)
                    m = std::min (m, v);
                noise[(std::size_t) b] = std::max (1.0e-20, m * 2.0); // the minimum under-reads the mean noise
                const auto post = power / noise[(std::size_t) b];
                const auto prior = std::max (0.003, 0.98 * prevClean[(std::size_t) b] / noise[(std::size_t) b] + 0.02 * std::max (0.0, post - 1.0));
                const auto gain = std::max (0.06, prior / (1.0 + prior));
                prevClean[(std::size_t) b] = gain * gain * power;
                const auto k = mix >= 0.0 ? (1.0 - mix) + mix * gain : (1.0 + mix) + (-mix) * (1.0 - gain);
                re[(std::size_t) b] *= k;
                im[(std::size_t) b] *= k;
            }
            ++frame;
        }, progress, (double) c / channels, 1.0 / channels);
        if (stop (progress))
            return;
        auto& ch = doc.audio.channels[(std::size_t) c];
        for (auto i = r.start; i < r.end; ++i)
            ch[(std::size_t) i] = out[(std::size_t) (i - from)];
    }
    report (progress, 1.0);
}

// --- match spectrum --------------------------------------------------------------------------------------------------
namespace
{
std::vector<double> longTermSpectrum (const Audio& audio, Range r, int size)
{
    const auto bins = size / 2 + 1;
    std::vector<double> sum ((std::size_t) bins, 0.0);
    const auto window = sqrtHann (size);
    std::vector<double> re ((std::size_t) size), im ((std::size_t) size);
    int frames = 0;
    for (auto start = r.start; start + size <= r.end || start == r.start; start += size / 2)
    {
        for (const auto& ch : audio.channels)
        {
            for (int i = 0; i < size; ++i)
            {
                const auto k = start + i;
                re[(std::size_t) i] = (k < r.end ? (double) ch[(std::size_t) k] * window[(std::size_t) i] : 0.0);
                im[(std::size_t) i] = 0.0;
            }
            fftInPlace (re, im, false);
            for (int b = 0; b < bins; ++b)
                sum[(std::size_t) b] += re[(std::size_t) b] * re[(std::size_t) b] + im[(std::size_t) b] * im[(std::size_t) b];
            ++frames;
        }
        if (start + size > r.end)
            break;
    }
    for (auto& v : sum)
        v = std::sqrt (v / std::max (1, frames));
    return sum;
}

std::vector<double> smoothOctaves (const std::vector<double>& v, double octaves)
{
    const auto n = (int) v.size();
    std::vector<double> out ((std::size_t) n);
    const auto f = std::pow (2.0, octaves * 0.5);
    std::vector<double> prefix ((std::size_t) n + 1, 0.0);
    for (int i = 0; i < n; ++i)
        prefix[(std::size_t) i + 1] = prefix[(std::size_t) i] + v[(std::size_t) i];
    for (int i = 0; i < n; ++i)
    {
        const auto lo = std::max (0, (int) std::floor (i / f)), hi = std::min (n - 1, std::max (i, (int) std::ceil (i * f)));
        out[(std::size_t) i] = (prefix[(std::size_t) hi + 1] - prefix[(std::size_t) lo]) / (double) (hi - lo + 1);
    }
    return out;
}
} // namespace

void matchSpectrum (Document& doc, Range r, const Audio& reference, double amount, double smoothingOctaves, Progress* progress)
{
    r = orAll (doc, r);
    if (r.empty() || reference.empty())
        return;
    constexpr int size = 8192;
    const auto ref = conform (reference, doc.audio.numChannels(), doc.audio.sampleRate);
    const auto mine = smoothOctaves (longTermSpectrum (doc.audio, r, size), smoothingOctaves);
    const auto theirs = smoothOctaves (longTermSpectrum (ref, { 0, ref.frames() }, size), smoothingOctaves);
    // the correction, limited to +-24 dB and with the overall level kept (the median correction is 0 dB)
    const auto bins = size / 2 + 1;
    std::vector<double> db ((std::size_t) bins);
    for (int b = 0; b < bins; ++b)
        db[(std::size_t) b] = std::clamp (20.0 * std::log10 ((theirs[(std::size_t) b] + 1.0e-9) / (mine[(std::size_t) b] + 1.0e-9)), -24.0, 24.0);
    auto sorted = db;
    std::nth_element (sorted.begin(), sorted.begin() + (std::ptrdiff_t) (bins / 2), sorted.end());
    const auto median = sorted[(std::size_t) bins / 2];
    std::vector<double> gains ((std::size_t) bins);
    for (int b = 0; b < bins; ++b)
        gains[(std::size_t) b] = dbToGain ((db[(std::size_t) b] - median) * clamp01 (amount));
    applyGains (doc, r, gains, size, 1.0, progress);
    report (progress, 1.0);
}

// --- claw machine ----------------------------------------------------------------------------------------------------
namespace
{
// the audio of `a` [from, to) as its own Audio
Audio slice (const Audio& a, std::int64_t from, std::int64_t to) { return copy (a, { from, to }); }

void append (Audio& dst, const Audio& src)
{
    if (dst.channels.empty())
    {
        dst = src;
        return;
    }
    for (std::size_t c = 0; c < dst.channels.size(); ++c)
        dst.channels[c].insert (dst.channels[c].end(), src.channels[std::min (c, src.channels.size() - 1)].begin(),
                                src.channels[std::min (c, src.channels.size() - 1)].end());
}

// `part` brought to `frames` frames: resampled (pitch follows) or time-stretched (pitch kept)
Audio refit (const Audio& part, std::int64_t frames, bool resampleIt)
{
    if (part.empty() || frames <= 0)
        return silence (std::max (1, part.numChannels()), part.sampleRate, std::max<std::int64_t> (0, frames));
    if (frames == part.frames())
        return part;
    Document d;
    d.audio = part;
    const auto ratio = (double) frames / (double) part.frames();
    if (resampleIt || part.frames() < 256)
        changeSpeed (d, {}, ratio);
    else
        timeStretch (d, {}, ratio);
    for (auto& ch : d.audio.channels)
        ch.resize ((std::size_t) frames, 0.0f);
    return d.audio;
}

// short fades at both ends of a piece (no clicks where pieces are joined)
void edgeFades (Audio& a, std::int64_t frames)
{
    const auto n = a.frames();
    const auto f = std::min (frames, n / 4);
    fadeFrames (a, 0, f, true);
    fadeFrames (a, n - f, n, false);
}

// period boundaries in [r.start, r.end): the markers inside (when snapping and there are any), else a grid
std::vector<std::int64_t> periodStarts (const Document& doc, Range r, std::int64_t period, bool snapToMarkers)
{
    std::vector<std::int64_t> starts { r.start };
    if (snapToMarkers)
        for (const auto& m : doc.markers)
            if (m.start > r.start && m.start < r.end)
                starts.push_back (m.start);
    if (starts.size() == 1)
        for (auto p = r.start + period; p < r.end; p += period)
            starts.push_back (p);
    std::sort (starts.begin(), starts.end());
    starts.erase (std::unique (starts.begin(), starts.end()), starts.end());
    return starts;
}
} // namespace

void clawMachine (Document& doc, Range r, const ClawSettings& s, double bpm, Progress* progress)
{
    r = orAll (doc, r);
    if (r.empty() || bpm <= 0.0)
        return;
    const auto sr = doc.audio.sampleRate;
    const auto period = std::max<std::int64_t> (16, (std::int64_t) std::llround (s.periodBeats * 60.0 / bpm * sr));
    const auto of = std::max (1, s.trashOf);
    const auto part = std::clamp (s.trashPart, 1, of);
    const auto starts = periodStarts (doc, r, period, s.snapToMarkers);
    const auto fade = (std::int64_t) (sr * 0.002);
    Audio out;
    out.sampleRate = sr;
    for (std::size_t i = 0; i < starts.size(); ++i)
    {
        if (stop (progress))
            return;
        report (progress, (double) i / (double) starts.size());
        const auto a = starts[i], b = i + 1 < starts.size() ? starts[i + 1] : r.end;
        const auto len = b - a;
        // the n-th of m equal parts goes to the trash
        const auto ta = a + len * (part - 1) / of, tb = a + len * part / of;
        auto kept = slice (doc.audio, a, ta);
        auto rest = slice (doc.audio, tb, b);
        if (! kept.empty())
            edgeFades (kept, fade);
        if (! rest.empty())
            edgeFades (rest, fade);
        Audio piece;
        piece.sampleRate = sr;
        piece.channels.assign ((std::size_t) doc.audio.numChannels(), {});
        append (piece, kept);
        append (piece, rest);
        // compensation: the kept audio stretched back towards the period's length
        const auto target = piece.frames() + (std::int64_t) std::llround ((double) (tb - ta) * clamp01 (s.compensation));
        if (target != piece.frames() && piece.frames() > 0)
            piece = refit (piece, target, s.resample);
        if (out.channels.empty())
            out.channels.assign ((std::size_t) doc.audio.numChannels(), {});
        append (out, piece);
    }
    if (out.channels.empty())
        return;
    // replace the range with the result (markers inside are dropped, the rest follow)
    erase (doc, r);
    insert (doc, r.start, out);
    report (progress, 1.0);
}

// --- drum(loop) stretch -----------------------------------------------------------------------------------------------
// Slices (markers inside the range, or onsets) keep their own speed and move to their stretched start; what is
// missing after a slice (stretching longer) is filled per the fill method, what is too long (shorter) is cut with a
// tail envelope. The result: a drum loop at a new tempo whose hits keep their attacks, like FL's drumloop stretcher.
void drumloopStretch (Document& doc, Range r, const DrumStretchSettings& s, Progress* progress)
{
    r = orAll (doc, r);
    if (r.length() < 64 || ! (s.ratio > 0.0))
        return;
    const auto sr = doc.audio.sampleRate;
    const auto channels = doc.audio.numChannels();
    // the slice starts
    std::vector<std::int64_t> starts { r.start };
    if (! s.autoSlice)
        for (const auto& m : doc.markers)
            if (m.start > r.start && m.start < r.end)
                starts.push_back (m.start);
    if (starts.size() == 1)
    {
        const auto part = copy (doc.audio, r);
        for (auto f : detectOnsets (part, (float) std::clamp (s.transient, 0.05, 1.0)))
            if (f > (std::int64_t) (sr * 0.01))
                starts.push_back (r.start + f);
    }
    std::sort (starts.begin(), starts.end());
    starts.erase (std::unique (starts.begin(), starts.end()), starts.end());

    const auto newLength = std::max<std::int64_t> (1, (std::int64_t) std::llround ((double) r.length() * s.ratio));
    Audio out = silence (channels, sr, newLength);
    const auto pitch = std::pow (2.0, s.pitchSemitones / 12.0);
    const auto fade = (std::int64_t) (sr * 0.003);
    Rng rng (777);

    for (std::size_t i = 0; i < starts.size(); ++i)
    {
        if (stop (progress))
            return;
        report (progress, (double) i / (double) starts.size());
        const auto a = starts[i], b = i + 1 < starts.size() ? starts[i + 1] : r.end;
        const auto at = (std::int64_t) std::llround ((double) (a - r.start) * s.ratio);
        const auto slotEnd = i + 1 < starts.size() ? (std::int64_t) std::llround ((double) (b - r.start) * s.ratio) : newLength;
        const auto slot = std::max<std::int64_t> (1, slotEnd - at);
        auto piece = slice (doc.audio, a, b);
        if (std::abs (pitch - 1.0) > 1.0e-6) // the slice's pitch (and so its speed) changes
            piece = refit (piece, std::max<std::int64_t> (1, (std::int64_t) std::llround ((double) piece.frames() / pitch)), true);
        const auto len = piece.frames();

        Audio filled = piece;
        if (slot > len)
        {
            const auto missing = slot - len;
            switch (s.fill)
            {
                case DrumStretchSettings::Fill::none:
                    break;
                case DrumStretchSettings::Fill::stretch:
                    filled = refit (piece, slot, false);
                    break;
                case DrumStretchSettings::Fill::echoGrains:
                case DrumStretchSettings::Fill::pogo:
                {
                    // grains from the slice's end (or its start with Loop), repeated, each a little quieter
                    const auto grainLen = std::clamp<std::int64_t> (len / 2, std::min<std::int64_t> (len, 64), (std::int64_t) (sr * 0.08));
                    auto grain = s.loop ? slice (piece, 0, grainLen) : slice (piece, len - grainLen, len);
                    edgeFades (grain, std::max<std::int64_t> (8, grainLen / 4));
                    Audio tailAudio = silence (channels, sr, missing + grainLen);
                    double level = 1.0;
                    bool backwards = false;
                    const auto hop = std::max<std::int64_t> (1, grainLen / 2);
                    for (std::int64_t p = 0; p < missing; p += hop)
                    {
                        level *= 0.92;
                        const auto presence = s.randomPresence ? 0.4 + 0.6 * rng.uniform() : 1.0;
                        for (int c = 0; c < channels; ++c)
                            for (std::int64_t k = 0; k < grainLen && p + k < tailAudio.frames(); ++k)
                            {
                                const auto src = backwards ? grainLen - 1 - k : k;
                                tailAudio.channels[(std::size_t) c][(std::size_t) (p + k)] += (float) (grain.channels[(std::size_t) c][(std::size_t) src] * level * presence * 0.7);
                            }
                        if (s.fill == DrumStretchSettings::Fill::pogo)
                            backwards = ! backwards;
                    }
                    for (auto& ch : tailAudio.channels)
                        ch.resize ((std::size_t) missing);
                    // the joint: a short crossfade from the slice's end into the grains
                    const auto x = std::min<std::int64_t> (fade * 2, std::min (len, missing));
                    fadeFrames (tailAudio, 0, x, true);
                    append (filled, tailAudio);
                    if (s.blur)
                    {
                        // a gentle low-pass over the fill (the grains blend into a smoother tail)
                        for (auto& ch : filled.channels)
                        {
                            double lp = ch[(std::size_t) len - 1];
                            for (auto k = len; k < (std::int64_t) ch.size(); ++k)
                                ch[(std::size_t) k] = (float) (lp += (ch[(std::size_t) k] - lp) * 0.35);
                        }
                    }
                    break;
                }
            }
        }
        // the tail envelope over what stands past the slice's natural end (or the cut when the slot is shorter)
        for (auto& ch : filled.channels)
            ch.resize ((std::size_t) slot, 0.0f);
        const auto smoothing = (std::int64_t) (std::clamp (s.tailSmoothing, 0.05, 2.0) * 0.02 * sr);
        if (s.tail == DrumStretchSettings::Tail::append && slot < len)
        {
            // append: the slice's own ending is played at the cut (crossfaded in), instead of a fade-out
            const auto x = std::min<std::int64_t> (smoothing, slot / 2);
            for (int c = 0; c < channels; ++c)
                for (std::int64_t k = 0; k < x; ++k)
                {
                    const auto t = ((double) k + 0.5) / (double) x;
                    auto& v = filled.channels[(std::size_t) c][(std::size_t) (slot - x + k)];
                    v = (float) (v * (1.0 - t) + piece.channels[(std::size_t) c][(std::size_t) (len - x + k)] * t);
                }
            fadeFrames (filled, slot - std::min<std::int64_t> (fade, slot), slot, false);
        }
        else if (s.tail != DrumStretchSettings::Tail::none)
        {
            // smooth fade: over the extension (stretching) or the last `smoothing` frames of the slot (shortening)
            const auto fa = slot > len ? std::min (len, slot) : std::max<std::int64_t> (0, slot - smoothing);
            const auto span = std::max<std::int64_t> (1, slot - fa);
            const auto slope = std::clamp (s.tailSlope, 0.0, 2.0);
            for (auto& ch : filled.channels)
                for (auto k = fa; k < slot; ++k)
                {
                    const auto t = (double) (k - fa) / (double) span;
                    ch[(std::size_t) k] *= (float) std::pow (1.0 - t, 0.25 + slope);
                }
        }
        else if (slot < len)
            fadeFrames (filled, slot - std::min<std::int64_t> (fade, slot), slot, false);
        fadeFrames (filled, 0, std::min<std::int64_t> (32, slot), true);
        for (int c = 0; c < channels; ++c)
            for (std::int64_t k = 0; k < slot && at + k < newLength; ++k)
                out.channels[(std::size_t) c][(std::size_t) (at + k)] += filled.channels[(std::size_t) c][(std::size_t) k];
    }

    if (s.insertTails)
    {
        // markers follow the slices
        std::vector<Marker> kept;
        for (auto m : doc.markers)
        {
            auto map = [&] (std::int64_t x) {
                if (x <= r.start)
                    return x;
                if (x >= r.end)
                    return x + newLength - r.length();
                return r.start + (std::int64_t) std::llround ((double) (x - r.start) * s.ratio);
            };
            m.start = map (m.start);
            m.end = m.end == m.start ? m.start : map (m.end);
            kept.push_back (m);
        }
        const auto markers = kept;
        const auto loopS = doc.loopStart, loopE = doc.loopEnd;
        erase (doc, r);
        insert (doc, r.start, out);
        doc.markers = markers;
        if (loopS >= 0)
        {
            auto map = [&] (std::int64_t x) { return x <= r.start ? x : x >= r.end ? x + newLength - r.length() : r.start + (std::int64_t) std::llround ((double) (x - r.start) * s.ratio); };
            doc.loopStart = map (loopS);
            doc.loopEnd = map (loopE);
        }
        sortMarkers (doc);
    }
    else
    {
        // pasted over: the original length stays (the result is cut or padded with the original after it)
        for (int c = 0; c < channels; ++c)
            for (std::int64_t k = 0; k < r.length(); ++k)
                doc.audio.channels[(std::size_t) c][(std::size_t) (r.start + k)] = k < newLength ? out.channels[(std::size_t) c][(std::size_t) k] : doc.audio.channels[(std::size_t) c][(std::size_t) (r.start + k)];
    }
    report (progress, 1.0);
}

// --- loop tuner ----------------------------------------------------------------------------------------------------
namespace
{
// the period (frames) of the strongest pitch around `at` (autocorrelation, 40..1000 Hz); 0 if none is clear
std::int64_t periodAt (const Audio& a, std::int64_t at)
{
    const auto sr = a.sampleRate;
    const auto win = (std::int64_t) (sr * 0.04);
    const auto lo = (std::int64_t) (sr / 1000.0), hi = (std::int64_t) (sr / 40.0);
    const auto from = std::max<std::int64_t> (0, at - win), to = std::min (a.frames(), at + win);
    if (to - from < 2 * hi)
        return 0;
    std::vector<double> m ((std::size_t) (to - from));
    for (auto i = from; i < to; ++i)
    {
        double v = 0.0;
        for (const auto& ch : a.channels)
            v += ch[(std::size_t) i];
        m[(std::size_t) (i - from)] = v;
    }
    double best = 0.0, energy = 0.0;
    std::int64_t lag = 0;
    const auto n = (std::int64_t) m.size();
    for (std::int64_t i = 0; i + hi < n; ++i)
        energy += m[(std::size_t) i] * m[(std::size_t) i];
    if (energy <= 1.0e-12)
        return 0;
    for (auto l = lo; l <= hi; ++l)
    {
        double acc = 0.0;
        for (std::int64_t i = 0; i + hi < n; ++i)
            acc += m[(std::size_t) i] * m[(std::size_t) (i + l)];
        if (acc > best)
        {
            best = acc;
            lag = l;
        }
    }
    return best / energy > 0.5 ? lag : 0;
}

// the position near `at` (within `radius`) where the audio best continues the audio just before `ref`
std::int64_t bestMatch (const Audio& a, std::int64_t at, std::int64_t ref, std::int64_t radius, std::int64_t span)
{
    double best = 1.0e300;
    auto pos = at;
    for (auto p = at - radius; p <= at + radius; ++p)
    {
        if (p - span < 0 || p >= a.frames() || ref - span < 0)
            continue;
        double d = 0.0;
        for (const auto& ch : a.channels)
            for (std::int64_t k = 1; k <= span; ++k)
            {
                const auto e = (double) ch[(std::size_t) (p - k)] - ch[(std::size_t) (ref - k)];
                d += e * e;
            }
        if (d < best)
        {
            best = d;
            pos = p;
        }
    }
    return pos;
}
} // namespace

void tuneLoop (Document& doc, Range r, const LoopTuneSettings& s, Progress* progress)
{
    Range loop = doc.hasLoop() ? Range { doc.loopStart, doc.loopEnd } : orAll (doc, r);
    if (! r.empty() && ! doc.hasLoop())
        loop = r;
    loop = clamp (doc, loop);
    if (loop.length() < 64)
        return;
    const auto sr = doc.audio.sampleRate;
    report (progress, 0.1);
    // 1. the loop end moves (by up to a pitch period, with snap > 1) to where the audio best continues into the start
    if (s.periodSnap > 1)
    {
        const auto period = periodAt (doc.audio, loop.end);
        const auto radius = period > 0 ? std::min<std::int64_t> (period, (std::int64_t) (sr * 0.025)) * (s.periodSnap - 1) / 6
                                       : (std::int64_t) (sr * 0.001 * (s.periodSnap - 1));
        if (radius > 0)
        {
            const auto span = std::max<std::int64_t> (16, period > 0 ? period : (std::int64_t) (sr * 0.005));
            // compare what precedes the end with what precedes the start (the continuation must match)
            const auto e = bestMatch (doc.audio, loop.end, loop.start, radius, std::min (span, loop.start));
            if (e - loop.start > 64)
                loop.end = e;
        }
        loop.start = nearestZeroCrossing (doc.audio, loop.start, (std::int64_t) (sr * 0.001));
    }
    report (progress, 0.4);
    // 2. crossfade: the end of the loop fades into the audio that comes before the loop start (so playing the end
    //    leads into the start the way the original audio led into it). Without audio before the start, the start
    //    itself is crossfaded with the end (an equal-power blend over the joint).
    const auto frames = doc.audio.frames();
    const auto usePre = loop.start >= frames - loop.end; // more audio before the start than after the end
    const auto available = usePre ? loop.start : frames - loop.end;
    const auto maxFade = std::min<std::int64_t> (loop.length() / 2, std::max<std::int64_t> (available, 0));
    const auto fadeLen = (std::int64_t) std::llround (clamp01 (s.crossfade) * (double) maxFade);
    if (fadeLen > 8)
    {
        const auto tension = (float) (clamp01 (s.tension) * 2.0 - 1.0) * 0.8f;
        const auto blurOrder = std::clamp (s.blurOrder, 1, 25);
        const auto gain = blurOrder > 1 ? std::clamp (s.blurGain, 0.5, 1.5) : 1.0;
        for (auto& ch : doc.audio.channels)
        {
            // usePre: the loop's end fades into the audio that led into the loop start;
            // else: the loop's start fades in from the audio that followed the loop end
            const auto at = usePre ? loop.end - fadeLen : loop.start;
            const auto other = usePre ? loop.start - fadeLen : loop.end;
            std::vector<float> mine (ch.begin() + (std::ptrdiff_t) at, ch.begin() + (std::ptrdiff_t) (at + fadeLen));
            for (std::int64_t k = 0; k < fadeLen; ++k)
            {
                const auto t0 = tensionCurve (((double) k + 0.5) / (double) fadeLen, tension);
                const auto towardsOther = usePre ? t0 : 1.0 - t0; // share of `other` in the blend
                // blur: several crossfades of slightly different shapes averaged (the seam is spread out)
                double blend = 0.0;
                for (int o = 0; o < blurOrder; ++o)
                {
                    const auto spread = 1.0 + std::clamp (s.blurScale, -1.0, 1.0) * (double) o / (double) std::max (1, blurOrder);
                    const auto t = std::clamp (0.5 + (towardsOther - 0.5) * spread, 0.0, 1.0);
                    blend += std::sqrt (1.0 - t) * mine[(std::size_t) k] + std::sqrt (t) * ch[(std::size_t) (other + k)];
                }
                ch[(std::size_t) (at + k)] = (float) (blend / blurOrder * gain);
            }
        }
    }
    doc.loopStart = loop.start;
    doc.loopEnd = loop.end;
    report (progress, 1.0);
}
} // namespace lynx::audioedit
