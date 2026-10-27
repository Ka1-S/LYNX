#include "core/AudioEdit.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <numeric>

#include "core/Instruments.h"
#include "core/TimeStretch.h"

namespace lynx::audioedit
{
namespace
{
constexpr double pi = 3.141592653589793;

std::int64_t clampFrame (std::int64_t f, std::int64_t frames) noexcept
{
    return std::clamp<std::int64_t> (f, 0, frames);
}

// How a position moves when the range [a, b) is replaced by `newLength` frames: inside it scales, after it shifts.
std::int64_t mapPosition (std::int64_t x, std::int64_t a, std::int64_t b, std::int64_t newLength) noexcept
{
    if (x <= a)
        return x;
    if (x >= b)
        return x + newLength - (b - a);
    if (b == a)
        return x;
    return a + (std::int64_t) std::llround ((double) (x - a) * (double) newLength / (double) (b - a));
}

// Markers/regions/loop follow a replacement of [a, b) by `newLength` frames. With `dropInside` (a deletion), markers
// strictly inside the range are removed; regions are clipped and removed when nothing is left.
void remapMarkers (Document& doc, std::int64_t a, std::int64_t b, std::int64_t newLength, bool dropInside)
{
    std::vector<Marker> kept;
    kept.reserve (doc.markers.size());

    for (auto m : doc.markers)
    {
        const auto wasRegion = m.isRegion();

        if (! wasRegion)
        {
            if (dropInside && m.start > a && m.start < b)
                continue;
            m.start = m.end = mapPosition (m.start, a, b, newLength);
        }
        else
        {
            m.start = mapPosition (m.start, a, b, newLength);
            m.end = mapPosition (m.end, a, b, newLength);
            if (m.end <= m.start)
                continue;
        }

        kept.push_back (std::move (m));
    }

    doc.markers = std::move (kept);

    if (doc.hasLoop())
    {
        doc.loopStart = mapPosition (doc.loopStart, a, b, newLength);
        doc.loopEnd = mapPosition (doc.loopEnd, a, b, newLength);
        if (doc.loopEnd <= doc.loopStart)
            doc.loopStart = doc.loopEnd = -1;
    }
}

// Replaces [r.start, r.end) of every channel with `with` (already in the document's format).
void replaceFrames (Document& doc, Range r, const Audio& with, bool dropMarkersInside)
{
    for (std::size_t c = 0; c < doc.audio.channels.size(); ++c)
    {
        auto& ch = doc.audio.channels[c];
        const auto& src = with.channels.empty() ? std::vector<float> {} : with.channels[std::min (c, with.channels.size() - 1)];
        ch.erase (ch.begin() + r.start, ch.begin() + r.end);
        ch.insert (ch.begin() + r.start, src.begin(), src.end());
    }

    remapMarkers (doc, r.start, r.end, with.frames(), dropMarkersInside);
}

template <typename Fn>
void forEachSample (Document& doc, Range r, Fn&& fn)
{
    r = orAll (doc, r);
    for (std::size_t c = 0; c < doc.audio.channels.size(); ++c)
        for (auto i = r.start; i < r.end; ++i)
            fn ((int) c, i - r.start, doc.audio.channels[c][(std::size_t) i]);
}

double sinc (double x) noexcept
{
    if (std::abs (x) < 1.0e-9)
        return 1.0;
    return std::sin (pi * x) / (pi * x);
}

// Blackman window over [-1, 1]
double blackman (double t) noexcept
{
    if (t <= -1.0 || t >= 1.0)
        return 0.0;
    const auto u = (t + 1.0) * 0.5; // 0..1
    return 0.42 - 0.5 * std::cos (2.0 * pi * u) + 0.08 * std::cos (4.0 * pi * u);
}

void fft (std::vector<std::complex<double>>& a)
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
        const auto angle = -2.0 * pi / (double) len;
        const std::complex<double> wl (std::cos (angle), std::sin (angle));
        for (std::size_t i = 0; i < n; i += len)
        {
            std::complex<double> w (1.0, 0.0);
            for (std::size_t k = 0; k < len / 2; ++k)
            {
                const auto u = a[i + k];
                const auto v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

std::shared_ptr<SampleData> toSampleData (const Audio& a)
{
    auto s = std::make_shared<SampleData>();
    s->sampleRate = a.sampleRate;
    if (! a.channels.empty())
        s->left = a.channels[0];
    if (a.channels.size() > 1)
        s->right = a.channels[1];
    return s;
}
} // namespace

Range clamp (const Document& doc, Range r) noexcept
{
    const auto n = doc.audio.frames();
    r.start = clampFrame (r.start, n);
    r.end = clampFrame (r.end, n);
    if (r.end < r.start)
        std::swap (r.start, r.end);
    return r;
}

Range orAll (const Document& doc, Range r) noexcept
{
    r = clamp (doc, r);
    return r.empty() ? Range { 0, doc.audio.frames() } : r;
}

Audio silence (int channels, double sampleRate, std::int64_t frames)
{
    Audio a;
    a.sampleRate = sampleRate;
    a.channels.assign ((std::size_t) std::max (1, channels), std::vector<float> ((std::size_t) std::max<std::int64_t> (0, frames), 0.0f));
    return a;
}

Audio conform (const Audio& clip, int channels, double sampleRate)
{
    Audio out;
    out.sampleRate = sampleRate;
    channels = std::max (1, channels);

    if (clip.channels.empty())
    {
        out.channels.assign ((std::size_t) channels, {});
        return out;
    }

    // channels first
    if (clip.numChannels() == channels)
        out.channels = clip.channels;
    else if (channels == 1)
    {
        std::vector<float> mono (clip.channels[0].size());
        for (std::size_t i = 0; i < mono.size(); ++i)
        {
            float sum = 0.0f;
            for (const auto& ch : clip.channels)
                sum += ch[i];
            mono[i] = sum / (float) clip.numChannels();
        }
        out.channels = { std::move (mono) };
    }
    else
    {
        for (int c = 0; c < channels; ++c)
            out.channels.push_back (clip.channels[(std::size_t) std::min (c, clip.numChannels() - 1)]);
    }

    // then the rate
    if (sampleRate > 0.0 && clip.sampleRate > 0.0 && std::abs (clip.sampleRate - sampleRate) > 1.0e-6)
    {
        const auto frames = (std::int64_t) std::llround ((double) clip.frames() * sampleRate / clip.sampleRate);
        for (auto& ch : out.channels)
            ch = resampleChannel (ch, frames);
    }

    return out;
}

Audio copy (const Audio& audio, Range r)
{
    Audio out;
    out.sampleRate = audio.sampleRate;
    const auto n = audio.frames();
    r.start = clampFrame (r.start, n);
    r.end = clampFrame (r.end, n);
    for (const auto& ch : audio.channels)
        out.channels.emplace_back (ch.begin() + r.start, ch.begin() + std::max (r.start, r.end));
    return out;
}

void erase (Document& doc, Range r)
{
    r = clamp (doc, r);
    if (r.empty())
        return;
    replaceFrames (doc, r, silence (doc.audio.numChannels(), doc.audio.sampleRate, 0), true);
}

Audio cut (Document& doc, Range r)
{
    auto clip = copy (doc.audio, clamp (doc, r));
    erase (doc, r);
    return clip;
}

void insert (Document& doc, std::int64_t at, const Audio& clip)
{
    if (clip.empty())
        return;

    if (doc.audio.channels.empty())
    {
        doc.audio = clip;
        return;
    }

    const auto conformed = conform (clip, doc.audio.numChannels(), doc.audio.sampleRate);
    at = clampFrame (at, doc.audio.frames());
    replaceFrames (doc, { at, at }, conformed, false);
}

void pasteReplace (Document& doc, Range r, const Audio& clip)
{
    r = clamp (doc, r);
    if (doc.audio.channels.empty())
    {
        doc.audio = clip;
        return;
    }
    replaceFrames (doc, r, conform (clip, doc.audio.numChannels(), doc.audio.sampleRate), r.empty() ? false : true);
}

void pasteMix (Document& doc, std::int64_t at, const Audio& clip)
{
    if (clip.empty())
        return;

    if (doc.audio.channels.empty())
    {
        doc.audio = clip;
        return;
    }

    const auto conformed = conform (clip, doc.audio.numChannels(), doc.audio.sampleRate);
    at = clampFrame (at, doc.audio.frames());
    const auto end = at + conformed.frames();

    for (std::size_t c = 0; c < doc.audio.channels.size(); ++c)
    {
        auto& ch = doc.audio.channels[c];
        if ((std::int64_t) ch.size() < end)
            ch.resize ((std::size_t) end, 0.0f);
        const auto& src = conformed.channels[c];
        for (std::size_t i = 0; i < src.size(); ++i)
            ch[(std::size_t) at + i] += src[i];
    }
}

void trim (Document& doc, Range r)
{
    r = clamp (doc, r);
    if (r.empty())
        return;
    erase (doc, { r.end, doc.audio.frames() });
    erase (doc, { 0, r.start });
}

void clear (Document& doc, Range r)
{
    forEachSample (doc, r, [] (int, std::int64_t, float& s) { s = 0.0f; });
}

void insertSilence (Document& doc, std::int64_t at, std::int64_t frames)
{
    if (frames <= 0)
        return;
    insert (doc, at, silence (std::max (1, doc.audio.numChannels()), doc.audio.sampleRate, frames));
}

void deleteAfterLoop (Document& doc)
{
    if (doc.hasLoop())
        erase (doc, { doc.loopEnd, doc.audio.frames() });
}

float curveGain (Curve c, float x) noexcept
{
    x = std::clamp (x, 0.0f, 1.0f);
    switch (c)
    {
        case Curve::linear: return x;
        case Curve::smooth: return 0.5f - 0.5f * std::cos ((float) pi * x);
        case Curve::exponential: return x * x;
        case Curve::logarithmic: return 1.0f - (1.0f - x) * (1.0f - x);
    }
    return x;
}

void fadeIn (Document& doc, Range r, Curve c)
{
    r = orAll (doc, r);
    const auto n = r.length();
    if (n <= 1)
        return;
    forEachSample (doc, r, [&] (int, std::int64_t i, float& s) { s *= curveGain (c, (float) ((double) i / (double) (n - 1))); });
}

void fadeOut (Document& doc, Range r, Curve c)
{
    r = orAll (doc, r);
    const auto n = r.length();
    if (n <= 1)
        return;
    forEachSample (doc, r, [&] (int, std::int64_t i, float& s) { s *= curveGain (c, 1.0f - (float) ((double) i / (double) (n - 1))); });
}

void declickIn (Document& doc, Range r)
{
    r = orAll (doc, r);
    const auto n = std::min<std::int64_t> (r.length(), (std::int64_t) std::llround (declickSeconds * doc.audio.sampleRate));
    fadeIn (doc, { r.start, r.start + n }, Curve::smooth);
}

void declickOut (Document& doc, Range r)
{
    r = orAll (doc, r);
    const auto n = std::min<std::int64_t> (r.length(), (std::int64_t) std::llround (declickSeconds * doc.audio.sampleRate));
    fadeOut (doc, { r.end - n, r.end }, Curve::smooth);
}

float peak (const Audio& audio, Range r) noexcept
{
    const auto n = audio.frames();
    r.start = clampFrame (r.start, n);
    r.end = clampFrame (r.end, n);
    float p = 0.0f;
    for (const auto& ch : audio.channels)
        for (auto i = r.start; i < r.end; ++i)
            p = std::max (p, std::abs (ch[(std::size_t) i]));
    return p;
}

float rms (const Audio& audio, Range r) noexcept
{
    const auto n = audio.frames();
    r.start = clampFrame (r.start, n);
    r.end = clampFrame (r.end, n);
    if (r.end <= r.start || audio.channels.empty())
        return 0.0f;
    double sum = 0.0;
    for (const auto& ch : audio.channels)
        for (auto i = r.start; i < r.end; ++i)
            sum += (double) ch[(std::size_t) i] * ch[(std::size_t) i];
    return (float) std::sqrt (sum / (double) ((r.end - r.start) * audio.numChannels()));
}

void amp (Document& doc, Range r, float gain, float pan)
{
    pan = std::clamp (pan, -1.0f, 1.0f);
    const auto stereo = doc.audio.numChannels() == 2;
    const float g[2] = { gain * (stereo ? std::min (1.0f, 1.0f - pan) : 1.0f), gain * (stereo ? std::min (1.0f, 1.0f + pan) : 1.0f) };
    forEachSample (doc, r, [&] (int c, std::int64_t, float& s) { s *= g[std::min (c, 1)]; });
}

float normalize (Document& doc, Range r, float targetDb)
{
    r = orAll (doc, r);
    const auto p = peak (doc.audio, r);
    if (p <= 1.0e-9f)
        return 1.0f;
    const auto g = std::pow (10.0f, targetDb / 20.0f) / p;
    amp (doc, r, g);
    return g;
}

void reverse (Document& doc, Range r)
{
    r = orAll (doc, r);
    for (auto& ch : doc.audio.channels)
        std::reverse (ch.begin() + r.start, ch.begin() + r.end);

    // markers inside the range are mirrored too
    for (auto& m : doc.markers)
    {
        auto mirror = [&] (std::int64_t x) { return (x >= r.start && x <= r.end) ? r.start + r.end - x : x; };
        if (m.start >= r.start && m.end <= r.end)
        {
            const auto s = mirror (m.end), e = mirror (m.start);
            m.start = s;
            m.end = e;
        }
    }
    sortMarkers (doc);
}

void invert (Document& doc, Range r)
{
    forEachSample (doc, r, [] (int, std::int64_t, float& s) { s = -s; });
}

void removeDc (Document& doc, Range r)
{
    r = orAll (doc, r);
    if (r.empty())
        return;
    for (auto& ch : doc.audio.channels)
    {
        double sum = 0.0;
        for (auto i = r.start; i < r.end; ++i)
            sum += ch[(std::size_t) i];
        const auto mean = (float) (sum / (double) r.length());
        for (auto i = r.start; i < r.end; ++i)
            ch[(std::size_t) i] -= mean;
    }
}

void toMono (Document& doc, MonoSource source)
{
    if (doc.audio.numChannels() < 2)
        return;
    auto& l = doc.audio.channels[0];
    const auto& r = doc.audio.channels[1];
    if (source == MonoSource::right)
        l = r;
    else if (source == MonoSource::merged)
        for (std::size_t i = 0; i < l.size(); ++i)
            l[i] = 0.5f * (l[i] + r[i]);
    doc.audio.channels.resize (1);
}

void toStereo (Document& doc)
{
    if (doc.audio.numChannels() == 1)
        doc.audio.channels.push_back (doc.audio.channels[0]);
}

void swapChannels (Document& doc)
{
    if (doc.audio.numChannels() == 2)
        std::swap (doc.audio.channels[0], doc.audio.channels[1]);
}

std::vector<float> resampleChannel (const std::vector<float>& in, std::int64_t outFrames)
{
    std::vector<float> out ((std::size_t) std::max<std::int64_t> (0, outFrames), 0.0f);
    const auto n = (std::int64_t) in.size();
    if (n == 0 || outFrames <= 0)
        return out;
    if (outFrames == n)
        return in;

    const auto step = (double) n / (double) outFrames; // input frames per output frame
    const auto cutoff = std::min (1.0, 1.0 / step);   // band limit when shortening
    const auto halfWidth = 16.0 / cutoff;              // in input frames (16 zero crossings each side)

    for (std::int64_t o = 0; o < outFrames; ++o)
    {
        const auto t = ((double) o + 0.5) * step - 0.5; // centres map to centres
        const auto first = (std::int64_t) std::ceil (t - halfWidth);
        const auto last = (std::int64_t) std::floor (t + halfWidth);
        double sum = 0.0, weights = 0.0;
        for (auto k = first; k <= last; ++k)
        {
            const auto x = t - (double) k;
            const auto w = cutoff * sinc (cutoff * x) * blackman (x / halfWidth);
            weights += w;
            if (k >= 0 && k < n)
                sum += w * in[(std::size_t) k];
            else // edges: the nearest sample continues (no dip to zero)
                sum += w * in[(std::size_t) std::clamp<std::int64_t> (k, 0, n - 1)];
        }
        out[(std::size_t) o] = (float) (weights != 0.0 ? sum / weights : 0.0);
    }

    return out;
}

void resample (Document& doc, double newRate)
{
    if (! (newRate > 0.0) || doc.audio.sampleRate <= 0.0 || std::abs (newRate - doc.audio.sampleRate) < 1.0e-6)
        return;
    const auto k = newRate / doc.audio.sampleRate;
    const auto frames = (std::int64_t) std::llround ((double) doc.audio.frames() * k);
    for (auto& ch : doc.audio.channels)
        ch = resampleChannel (ch, frames);
    doc.audio.sampleRate = newRate;

    auto scale = [k] (std::int64_t x) { return (std::int64_t) std::llround ((double) x * k); };
    for (auto& m : doc.markers)
    {
        m.start = scale (m.start);
        m.end = scale (m.end);
    }
    if (doc.hasLoop())
    {
        doc.loopStart = scale (doc.loopStart);
        doc.loopEnd = scale (doc.loopEnd);
    }
}

void timeStretch (Document& doc, Range r, double ratio)
{
    r = orAll (doc, r);
    if (r.length() < 4 || ! (ratio > 0.0) || std::abs (ratio - 1.0) < 1.0e-9)
        return;

    const auto part = copy (doc.audio, r);
    const auto stretched = lynx::timeStretch (*toSampleData (part), ratio);
    const auto frames = (std::int64_t) std::llround ((double) r.length() * ratio);

    Audio with;
    with.sampleRate = doc.audio.sampleRate;
    with.channels.push_back (stretched->left);
    if (doc.audio.numChannels() > 1)
        with.channels.push_back (stretched->isStereo() ? stretched->right : stretched->left);
    for (auto& ch : with.channels)
        ch.resize ((std::size_t) frames, 0.0f);

    replaceFrames (doc, r, with, false);
}

void changeSpeed (Document& doc, Range r, double ratio)
{
    r = orAll (doc, r);
    if (r.empty() || ! (ratio > 0.0) || std::abs (ratio - 1.0) < 1.0e-9)
        return;
    auto part = copy (doc.audio, r);
    const auto frames = std::max<std::int64_t> (1, (std::int64_t) std::llround ((double) r.length() * ratio));
    for (auto& ch : part.channels)
        ch = resampleChannel (ch, frames);
    replaceFrames (doc, r, part, false);
}

void pitchShift (Document& doc, Range r, double semitones)
{
    r = orAll (doc, r);
    if (r.length() < 4 || std::abs (semitones) < 1.0e-9)
        return;
    const auto factor = std::pow (2.0, semitones / 12.0);
    const auto length = r.length();

    // longer by the factor without changing pitch, then squeezed back to the original length
    Document part;
    part.audio = copy (doc.audio, r);
    timeStretch (part, {}, factor);
    for (auto& ch : part.audio.channels)
        ch = resampleChannel (ch, length);
    replaceFrames (doc, r, part.audio, false);
}

void sortMarkers (Document& doc)
{
    std::stable_sort (doc.markers.begin(), doc.markers.end(), [] (const Marker& a, const Marker& b) { return a.start < b.start; });
}

void addMarker (Document& doc, std::int64_t at, std::string name)
{
    at = clampFrame (at, doc.audio.frames());
    doc.markers.push_back ({ at, at, std::move (name) });
    sortMarkers (doc);
}

void addRegion (Document& doc, Range r, std::string name)
{
    r = clamp (doc, r);
    if (r.empty())
        return;
    doc.markers.push_back ({ r.start, r.end, std::move (name) });
    sortMarkers (doc);
}

int removeMarkers (Document& doc, Range r)
{
    const auto before = doc.markers.size();
    std::erase_if (doc.markers, [&] (const Marker& m) {
        if (r.empty())
            return m.start == r.start;
        if (m.isRegion())
            return m.start < r.end && m.end > r.start;
        return m.start >= r.start && m.start < r.end;
    });
    return (int) (before - doc.markers.size());
}

void setLoop (Document& doc, Range r)
{
    r = clamp (doc, r);
    if (r.empty())
        doc.loopStart = doc.loopEnd = -1;
    else
    {
        doc.loopStart = r.start;
        doc.loopEnd = r.end;
    }
}

std::int64_t nearestZeroCrossing (const Audio& audio, std::int64_t frame, std::int64_t radius)
{
    const auto n = audio.frames();
    if (n < 2 || audio.channels.empty())
        return frame;
    frame = clampFrame (frame, n);

    auto value = [&] (std::int64_t i) {
        float s = 0.0f;
        for (const auto& ch : audio.channels)
            s += ch[(std::size_t) i];
        return s;
    };
    // a crossing "at i" = the sign changes between i - 1 and i (or the sample is exactly 0)
    auto crossesAt = [&] (std::int64_t i) {
        if (i <= 0 || i >= n)
            return i == 0 || i == n;
        const auto a = value (i - 1), b = value (i);
        return b == 0.0f || (a < 0.0f) != (b < 0.0f);
    };

    for (std::int64_t d = 0; d <= radius; ++d)
    {
        if (frame - d >= 0 && crossesAt (frame - d))
            return frame - d;
        if (frame + d <= n && crossesAt (frame + d))
            return frame + d;
    }
    return frame;
}

std::vector<std::int64_t> detectOnsets (const Audio& audio, float sensitivity)
{
    std::vector<std::int64_t> onsets;
    const auto n = audio.frames();
    if (n == 0 || audio.sampleRate <= 0.0)
        return onsets;

    // energy envelope in 5.8 ms hops (256 frames at 44.1 kHz); an onset = energy rising well above the recent
    // average and above an absolute floor; at least 50 ms between onsets
    const auto hop = std::max<std::int64_t> (16, (std::int64_t) std::llround (audio.sampleRate * 0.0058));
    const auto hops = (std::size_t) ((n + hop - 1) / hop);
    std::vector<double> energy (hops, 0.0);
    for (std::size_t h = 0; h < hops; ++h)
    {
        const auto from = (std::int64_t) h * hop, to = std::min (n, from + hop);
        double e = 0.0;
        for (const auto& ch : audio.channels)
            for (auto i = from; i < to; ++i)
                e += (double) ch[(std::size_t) i] * ch[(std::size_t) i];
        energy[h] = e / (double) ((to - from) * audio.numChannels());
    }

    sensitivity = std::clamp (sensitivity, 0.0f, 1.0f);
    const auto ratio = 8.0 - 6.0 * sensitivity;                    // dull: x8 jump, sharp: x2
    const auto floorDb = -30.0 - 20.0 * sensitivity;               // dull: -30 dB, sharp: -50 dB
    const auto floor = std::pow (10.0, floorDb / 10.0);
    const auto gap = (std::int64_t) std::llround (audio.sampleRate * 0.05 / (double) hop);
    std::int64_t last = -gap - 1;

    for (std::size_t h = 0; h < hops; ++h)
    {
        double recent = 0.0;
        const auto back = std::min<std::size_t> (h, 8);
        for (std::size_t k = 1; k <= back; ++k)
            recent += energy[h - k];
        recent = back > 0 ? recent / (double) back : 0.0;

        if (energy[h] > floor && energy[h] > ratio * recent + 1.0e-12 && (std::int64_t) h - last > gap)
        {
            // the attack starts inside this hop: the first sample above a tenth of the hop's peak
            const auto from = (std::int64_t) h * hop, to = std::min (n, from + hop);
            float p = 0.0f;
            for (const auto& ch : audio.channels)
                for (auto i = from; i < to; ++i)
                    p = std::max (p, std::abs (ch[(std::size_t) i]));
            auto at = from;
            for (auto i = from; i < to; ++i)
            {
                float v = 0.0f;
                for (const auto& ch : audio.channels)
                    v = std::max (v, std::abs (ch[(std::size_t) i]));
                if (v >= 0.1f * p)
                {
                    at = i;
                    break;
                }
            }
            onsets.push_back (at);
            last = (std::int64_t) h;
        }
    }
    return onsets;
}

// --- history -----------------------------------------------------------------------------------------------------
namespace
{
std::size_t documentBytes (const Document& d) noexcept
{
    return (std::size_t) d.audio.frames() * (std::size_t) std::max (1, d.audio.numChannels()) * sizeof (float) + d.markers.size() * sizeof (Marker);
}
} // namespace

History::History (std::size_t maxBytesToKeep) : maxBytes (maxBytesToKeep) {}

void History::reset (const Document& doc, std::string label)
{
    states.clear();
    states.push_back ({ std::make_shared<const Document> (doc), std::move (label) });
    position = 0;
}

void History::commit (const Document& doc, std::string label)
{
    if (states.empty())
    {
        reset (doc, std::move (label));
        return;
    }
    states.resize ((std::size_t) position + 1); // redo steps are dropped
    states.push_back ({ std::make_shared<const Document> (doc), std::move (label) });
    position = (int) states.size() - 1;

    while (states.size() > 2 && bytes() > maxBytes)
    {
        states.erase (states.begin());
        --position;
    }
}

const Document& History::undo()
{
    if (canUndo())
        --position;
    return current();
}

const Document& History::redo()
{
    if (canRedo())
        ++position;
    return current();
}

const Document& History::jumpTo (int index)
{
    position = std::clamp (index, 0, (int) states.size() - 1);
    return current();
}

std::size_t History::bytes() const noexcept
{
    std::size_t total = 0;
    for (const auto& s : states)
        total += documentBytes (*s.doc);
    return total;
}

// --- spectrogram ------------------------------------------------------------------------------------------------
Spectrogram spectrogram (const Audio& audio, Range r, int columns, int fftSize)
{
    Spectrogram s;
    s.sampleRate = audio.sampleRate;
    if (columns <= 0 || fftSize < 16 || (fftSize & (fftSize - 1)) != 0 || audio.channels.empty())
        return s;

    const auto n = audio.frames();
    r.start = clampFrame (r.start, n);
    r.end = clampFrame (r.end, n);
    s.columns = columns;
    s.bins = fftSize / 2;
    s.db.assign ((std::size_t) columns * (std::size_t) s.bins, -120.0f);
    if (r.empty())
        return s;

    std::vector<double> window ((std::size_t) fftSize);
    double windowSum = 0.0;
    for (int i = 0; i < fftSize; ++i)
    {
        window[(std::size_t) i] = 0.5 - 0.5 * std::cos (2.0 * pi * (double) i / (double) fftSize);
        windowSum += window[(std::size_t) i];
    }

    std::vector<std::complex<double>> buffer ((std::size_t) fftSize);
    const auto scale = 2.0 / windowSum; // a full-scale sine reads 0 dB
    const auto channels = (double) audio.numChannels();

    for (int c = 0; c < columns; ++c)
    {
        const auto centre = (double) r.start + ((double) c + 0.5) * (double) r.length() / (double) columns;
        const auto first = (std::int64_t) std::llround (centre) - fftSize / 2;
        for (int i = 0; i < fftSize; ++i)
        {
            const auto k = first + i;
            double v = 0.0;
            if (k >= 0 && k < n)
                for (const auto& ch : audio.channels)
                    v += ch[(std::size_t) k];
            buffer[(std::size_t) i] = { v / channels * window[(std::size_t) i], 0.0 };
        }
        fft (buffer);
        for (int b = 0; b < s.bins; ++b)
        {
            const auto magnitude = std::abs (buffer[(std::size_t) b]) * scale;
            s.db[(std::size_t) c * (std::size_t) s.bins + (std::size_t) b] = (float) std::max (-120.0, 20.0 * std::log10 (magnitude + 1.0e-12));
        }
    }
    return s;
}

// --- WAV ---------------------------------------------------------------------------------------------------------
namespace
{
void put16 (std::vector<std::uint8_t>& o, std::uint32_t v)
{
    o.push_back ((std::uint8_t) (v & 0xff));
    o.push_back ((std::uint8_t) ((v >> 8) & 0xff));
}

void put32 (std::vector<std::uint8_t>& o, std::uint32_t v)
{
    put16 (o, v & 0xffff);
    put16 (o, v >> 16);
}

void putTag (std::vector<std::uint8_t>& o, const char* tag)
{
    o.insert (o.end(), tag, tag + 4);
}

void set32 (std::vector<std::uint8_t>& o, std::size_t at, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        o[at + (std::size_t) i] = (std::uint8_t) ((v >> (8 * i)) & 0xff);
}

std::uint32_t get16 (const std::uint8_t* p) noexcept
{
    return (std::uint32_t) p[0] | ((std::uint32_t) p[1] << 8);
}

std::uint32_t get32 (const std::uint8_t* p) noexcept
{
    return get16 (p) | (get16 (p + 2) << 16);
}

// begins a chunk; returns where its size goes
std::size_t beginChunk (std::vector<std::uint8_t>& o, const char* tag)
{
    putTag (o, tag);
    put32 (o, 0);
    return o.size() - 4;
}

void endChunk (std::vector<std::uint8_t>& o, std::size_t sizeAt)
{
    set32 (o, sizeAt, (std::uint32_t) (o.size() - sizeAt - 4));
    if (o.size() % 2 != 0)
        o.push_back (0); // chunks are word aligned
}

bool parseMarkers (const std::uint8_t* data, std::size_t size, Document& out, std::size_t frames, bool& gotAny)
{
    struct Cue
    {
        std::uint32_t id = 0, position = 0, length = 0;
        std::string name;
    };
    std::vector<Cue> cues;
    gotAny = false;

    auto findCue = [&] (std::uint32_t id) -> Cue* {
        for (auto& c : cues)
            if (c.id == id)
                return &c;
        return nullptr;
    };

    std::size_t p = 12;
    // first pass: cue points; second: labels (they may come before the cue chunk)
    for (int pass = 0; pass < 2; ++pass)
    {
        p = 12;
        while (p + 8 <= size)
        {
            const auto* tag = data + p;
            const auto len = (std::size_t) get32 (data + p + 4);
            const auto body = p + 8;
            if (body + len > size)
                break;

            if (pass == 0 && std::memcmp (tag, "cue ", 4) == 0 && len >= 4)
            {
                const auto count = get32 (data + body);
                for (std::uint32_t i = 0; i < count && body + 4 + (i + 1) * 24 <= body + len; ++i)
                {
                    const auto* c = data + body + 4 + i * 24;
                    cues.push_back ({ get32 (c), get32 (c + 20), 0, {} });
                }
            }
            else if (pass == 0 && std::memcmp (tag, "smpl", 4) == 0 && len >= 36)
            {
                const auto loops = get32 (data + body + 28);
                if (loops > 0 && len >= 36 + 24)
                {
                    const auto* l = data + body + 36;
                    const auto start = (std::int64_t) get32 (l + 8), end = (std::int64_t) get32 (l + 12) + 1; // smpl end is inclusive
                    if (end > start && (std::size_t) end <= frames)
                    {
                        out.loopStart = start;
                        out.loopEnd = end;
                        gotAny = true;
                    }
                }
            }
            else if (pass == 1 && std::memcmp (tag, "LIST", 4) == 0 && len >= 4 && std::memcmp (data + body, "adtl", 4) == 0)
            {
                auto q = body + 4;
                while (q + 8 <= body + len)
                {
                    const auto sub = data + q;
                    const auto subLen = (std::size_t) get32 (data + q + 4);
                    const auto sb = q + 8;
                    if (sb + subLen > body + len)
                        break;
                    if ((std::memcmp (sub, "labl", 4) == 0 || std::memcmp (sub, "note", 4) == 0) && subLen >= 4)
                    {
                        if (auto* c = findCue (get32 (data + sb)); c != nullptr && std::memcmp (sub, "labl", 4) == 0)
                        {
                            const auto* s = reinterpret_cast<const char*> (data + sb + 4);
                            c->name.assign (s, strnlen (s, subLen - 4));
                        }
                    }
                    else if (std::memcmp (sub, "ltxt", 4) == 0 && subLen >= 8)
                    {
                        if (auto* c = findCue (get32 (data + sb)))
                            c->length = get32 (data + sb + 4);
                    }
                    q = sb + subLen + (subLen & 1);
                }
            }
            p = body + len + (len & 1);
        }
    }

    for (const auto& c : cues)
    {
        Marker m;
        m.start = std::min<std::int64_t> (c.position, (std::int64_t) frames);
        m.end = std::min<std::int64_t> (m.start + c.length, (std::int64_t) frames);
        m.name = c.name;
        out.markers.push_back (std::move (m));
        gotAny = true;
    }
    sortMarkers (out);
    return gotAny;
}
} // namespace

std::vector<std::uint8_t> encodeWav (const Document& doc, WavFormat format)
{
    const auto channels = (std::uint32_t) std::max (1, doc.audio.numChannels());
    const auto frames = (std::size_t) doc.audio.frames();
    const auto bytesPerSample = format == WavFormat::int16 ? 2u : format == WavFormat::int24 ? 3u : 4u;
    const auto rate = (std::uint32_t) std::llround (doc.audio.sampleRate);

    std::vector<std::uint8_t> o;
    o.reserve (frames * channels * bytesPerSample + 1024);
    putTag (o, "RIFF");
    put32 (o, 0);
    putTag (o, "WAVE");

    auto at = beginChunk (o, "fmt ");
    put16 (o, format == WavFormat::float32 ? 3 : 1);
    put16 (o, channels);
    put32 (o, rate);
    put32 (o, rate * channels * bytesPerSample);
    put16 (o, channels * bytesPerSample);
    put16 (o, bytesPerSample * 8);
    endChunk (o, at);

    at = beginChunk (o, "data");
    for (std::size_t i = 0; i < frames; ++i)
        for (std::uint32_t c = 0; c < channels; ++c)
        {
            const auto v = doc.audio.channels.empty() ? 0.0f : doc.audio.channels[std::min<std::size_t> (c, doc.audio.channels.size() - 1)][i];
            if (format == WavFormat::float32)
            {
                std::uint32_t bits;
                std::memcpy (&bits, &v, 4);
                put32 (o, bits);
            }
            else
            {
                const auto maxValue = format == WavFormat::int16 ? 32767.0 : 8388607.0;
                const auto s = (std::int32_t) std::llround (std::clamp ((double) v, -1.0, 1.0) * maxValue);
                const auto u = (std::uint32_t) s;
                o.push_back ((std::uint8_t) (u & 0xff));
                o.push_back ((std::uint8_t) ((u >> 8) & 0xff));
                if (format == WavFormat::int24)
                    o.push_back ((std::uint8_t) ((u >> 16) & 0xff));
            }
        }
    endChunk (o, at);

    if (doc.hasLoop())
    {
        at = beginChunk (o, "smpl");
        put32 (o, 0);                                                       // manufacturer
        put32 (o, 0);                                                       // product
        put32 (o, rate > 0 ? (std::uint32_t) (1000000000.0 / rate) : 0);   // sample period (ns)
        put32 (o, 60);                                                      // unity note
        put32 (o, 0);                                                       // pitch fraction
        put32 (o, 0);                                                       // SMPTE format
        put32 (o, 0);                                                       // SMPTE offset
        put32 (o, 1);                                                       // loops
        put32 (o, 0);                                                       // sampler data
        put32 (o, 0);                                                       // loop id
        put32 (o, 0);                                                       // forward
        put32 (o, (std::uint32_t) doc.loopStart);
        put32 (o, (std::uint32_t) (doc.loopEnd - 1));                       // inclusive
        put32 (o, 0);                                                       // fraction
        put32 (o, 0);                                                       // play count (infinite)
        endChunk (o, at);
    }

    if (! doc.markers.empty())
    {
        at = beginChunk (o, "cue ");
        put32 (o, (std::uint32_t) doc.markers.size());
        for (std::size_t i = 0; i < doc.markers.size(); ++i)
        {
            put32 (o, (std::uint32_t) i + 1);
            put32 (o, (std::uint32_t) doc.markers[i].start);
            putTag (o, "data");
            put32 (o, 0);
            put32 (o, 0);
            put32 (o, (std::uint32_t) doc.markers[i].start);
        }
        endChunk (o, at);

        at = beginChunk (o, "LIST");
        putTag (o, "adtl");
        for (std::size_t i = 0; i < doc.markers.size(); ++i)
        {
            const auto& m = doc.markers[i];
            if (! m.name.empty())
            {
                const auto sub = beginChunk (o, "labl");
                put32 (o, (std::uint32_t) i + 1);
                o.insert (o.end(), m.name.begin(), m.name.end());
                o.push_back (0);
                endChunk (o, sub);
            }
            if (m.isRegion())
            {
                const auto sub = beginChunk (o, "ltxt");
                put32 (o, (std::uint32_t) i + 1);
                put32 (o, (std::uint32_t) (m.end - m.start));
                putTag (o, "rgn ");
                put16 (o, 0);
                put16 (o, 0);
                put16 (o, 0);
                put16 (o, 0);
                endChunk (o, sub);
            }
        }
        endChunk (o, at);
    }

    set32 (o, 4, (std::uint32_t) (o.size() - 8));
    return o;
}

bool decodeWav (const std::uint8_t* data, std::size_t size, Document& out)
{
    if (size < 12 || std::memcmp (data, "RIFF", 4) != 0 || std::memcmp (data + 8, "WAVE", 4) != 0)
        return false;

    std::uint32_t format = 0, channels = 0, rate = 0, bits = 0;
    const std::uint8_t* samples = nullptr;
    std::size_t sampleBytes = 0;

    for (std::size_t p = 12; p + 8 <= size;)
    {
        const auto len = (std::size_t) get32 (data + p + 4);
        const auto body = p + 8;
        const auto avail = std::min (len, size - body);
        if (std::memcmp (data + p, "fmt ", 4) == 0 && avail >= 16)
        {
            format = get16 (data + body);
            channels = get16 (data + body + 2);
            rate = get32 (data + body + 4);
            bits = get16 (data + body + 14);
            if (format == 0xfffe && avail >= 26) // extensible: the sub-format's first word
                format = get16 (data + body + 24);
        }
        else if (std::memcmp (data + p, "data", 4) == 0)
        {
            samples = data + body;
            sampleBytes = avail;
        }
        p = body + len + (len & 1);
    }

    if (samples == nullptr || channels == 0 || channels > 8 || rate == 0)
        return false;
    if (! ((format == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) || (format == 3 && (bits == 32 || bits == 64))))
        return false;

    const auto bytes = bits / 8;
    const auto frames = sampleBytes / (bytes * channels);
    out = {};
    out.audio.sampleRate = rate;
    out.audio.channels.assign (std::min<std::uint32_t> (channels, 2), std::vector<float> (frames));

    for (std::size_t i = 0; i < frames; ++i)
        for (std::uint32_t c = 0; c < std::min<std::uint32_t> (channels, 2); ++c)
        {
            const auto* s = samples + (i * channels + c) * bytes;
            float v = 0.0f;
            if (format == 3 && bits == 32)
                std::memcpy (&v, s, 4);
            else if (format == 3)
            {
                double d;
                std::memcpy (&d, s, 8);
                v = (float) d;
            }
            else if (bits == 8)
                v = ((float) s[0] - 128.0f) / 128.0f;
            else if (bits == 16)
                v = (float) (std::int16_t) get16 (s) / 32768.0f;
            else if (bits == 24)
                v = (float) ((std::int32_t) ((std::uint32_t) s[0] << 8 | (std::uint32_t) s[1] << 16 | (std::uint32_t) s[2] << 24) >> 8) / 8388608.0f;
            else
                v = (float) ((double) (std::int32_t) get32 (s) / 2147483648.0);
            out.audio.channels[c][i] = v;
        }

    bool any = false;
    parseMarkers (data, size, out, frames, any);
    return true;
}

bool decodeWavMarkers (const std::uint8_t* data, std::size_t size, Document& out)
{
    if (size < 12 || std::memcmp (data, "RIFF", 4) != 0 || std::memcmp (data + 8, "WAVE", 4) != 0)
        return false;
    out.markers.clear();
    out.loopStart = out.loopEnd = -1;
    bool any = false;
    parseMarkers (data, size, out, (std::size_t) std::max<std::int64_t> (out.audio.frames(), 0), any);
    return any;
}

WavExtras decodeWavExtras (const std::uint8_t* data, std::size_t size, std::int64_t frames)
{
    WavExtras x;
    if (data == nullptr || size < 12 || frames <= 0 || std::memcmp (data, "RIFF", 4) != 0 || std::memcmp (data + 8, "WAVE", 4) != 0)
        return x;
    Document d;
    bool any = false;
    parseMarkers (data, size, d, (std::size_t) frames, any);
    for (const auto& m : d.markers)
        x.markerStarts.push_back (std::min (m.start, frames - 1));
    std::sort (x.markerStarts.begin(), x.markerStarts.end());
    for (std::size_t p = 12; p + 8 <= size;)
    {
        const auto len = (std::size_t) get32 (data + p + 4);
        const auto body = p + 8;
        if (body + len > size)
            break;
        if (std::memcmp (data + p, "acid", 4) == 0 && len >= 16)
        {
            const auto beats = get32 (data + body + 12);
            x.acidBeats = beats > 0 && beats < 65536 ? (float) beats : 0.0f;
        }
        p = body + len + (len & 1);
    }
    return x;
}
} // namespace lynx::audioedit
