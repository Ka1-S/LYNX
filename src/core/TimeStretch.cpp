#include "core/TimeStretch.h"

#include <algorithm>
#include <cmath>
#include <numeric>

#if defined(__clang__)
 #pragma clang diagnostic push
 #pragma clang diagnostic ignored "-Wpedantic"
 #pragma clang diagnostic ignored "-Wextra-semi"
 #pragma clang diagnostic ignored "-Wshadow"
 #pragma clang diagnostic ignored "-Wunused-parameter"
 #pragma clang diagnostic ignored "-Wsign-compare"
 #pragma clang diagnostic ignored "-Wimplicit-float-conversion"
 #pragma clang diagnostic ignored "-Wdeprecated-declarations"
#elif defined(__GNUC__)
 #pragma GCC diagnostic push
 #pragma GCC diagnostic ignored "-Wpedantic"
 #pragma GCC diagnostic ignored "-Wshadow"
 #pragma GCC diagnostic ignored "-Wunused-parameter"
 #pragma GCC diagnostic ignored "-Wsign-compare"
#elif defined(_MSC_VER)
 #pragma warning(push, 0)
#endif
#include "../../third_party/signalsmith/signalsmith-stretch.h"
#if defined(__clang__)
 #pragma clang diagnostic pop
#elif defined(__GNUC__)
 #pragma GCC diagnostic pop
#elif defined(_MSC_VER)
 #pragma warning(pop)
#endif

namespace lynx
{
namespace
{
constexpr double pi = 3.141592653589793;

struct Planar
{
    std::vector<std::vector<float>> ch;
    long frames() const { return ch.empty() ? 0 : (long) ch[0].size(); }
};

Planar planarOf (const SampleData& in)
{
    Planar p;
    const auto n = (std::size_t) in.frames();
    p.ch.emplace_back (in.leftData(), in.leftData() + n);
    if (in.isStereo())
        p.ch.emplace_back (in.rightData(), in.rightData() + n);
    return p;
}

std::shared_ptr<SampleData> sampleOf (Planar&& p, const SampleData& like)
{
    auto out = std::make_shared<SampleData>();
    out->sampleRate = like.sampleRate;
    out->left = std::move (p.ch[0]);
    if (p.ch.size() > 1)
        out->right = std::move (p.ch[1]);
    return out;
}

std::vector<float> monoOf (const Planar& p)
{
    std::vector<float> m (p.ch[0]);
    if (p.ch.size() > 1)
        for (std::size_t i = 0; i < m.size(); ++i)
            m[i] = 0.5f * (m[i] + p.ch[1][i]);
    return m;
}

double besselI0 (double x)
{
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 40; ++k)
    {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < 1e-12 * sum)
            break;
    }
    return sum;
}

// Kaiser-windowed sinc resampling of the whole signal to `outFrames` (used for ratios within 0.1 % of 1, where the
// cutoff stays at Nyquist): 64 taps, β = 9 (stop band beyond −90 dB). Polyphase: the kernel is tabulated for 512
// fractional positions (each row = the 64 taps for that position) and two neighbouring rows are blended, so each output
// frame is two contiguous dot products (vectorised) — a 3-minute stereo take resamples in about a second.
Planar resampleTo (const Planar& in, long outFrames)
{
    constexpr int half = 32, taps = 2 * half;
    constexpr int phases = 512;
    constexpr double beta = 9.0;
    const auto n = in.frames();
    const auto step = (double) n / (double) outFrames; // input frames per output frame
    const auto norm = 1.0 / besselI0 (beta);

    const auto kernel = [&] (double x) {
        const auto r = std::abs (x) / half;
        if (r >= 1.0)
            return 0.0;
        const auto w = besselI0 (beta * std::sqrt (1.0 - r * r)) * norm;
        return (x == 0.0 ? 1.0 : std::sin (pi * x) / (pi * x)) * w;
    };
    // row p, tap k: the weight of input frame i0 − half + 1 + k for the position i0 + p / phases
    std::vector<float> table ((std::size_t) ((phases + 1) * taps));
    for (int p = 0; p <= phases; ++p)
        for (int k = 0; k < taps; ++k)
            table[(std::size_t) (p * taps + k)] = (float) kernel ((double) (k - half + 1) - (double) p / phases);

    Planar out;
    for (const auto& c : in.ch)
    {
        std::vector<float> y ((std::size_t) outFrames);
        for (long j = 0; j < outFrames; ++j)
        {
            const auto pos = (double) j * step;
            const auto i0 = (long) std::floor (pos);
            const auto phase = (pos - (double) i0) * phases;
            const auto p = std::min (phases - 1, (int) phase);
            const auto g = (float) (phase - (double) p);
            const auto* k0 = table.data() + (std::size_t) (p * taps);
            const auto* k1 = k0 + taps;
            const auto first = i0 - half + 1;
            float a0 = 0.0f, a1 = 0.0f;
            if (first >= 0 && first + taps <= n)
            {
                const auto* x = c.data() + first;
                for (int k = 0; k < taps; ++k)
                {
                    a0 += x[k] * k0[k];
                    a1 += x[k] * k1[k];
                }
            }
            else
                for (int k = 0; k < taps; ++k)
                {
                    const auto i = first + k;
                    if (i >= 0 && i < n)
                    {
                        a0 += c[(std::size_t) i] * k0[k];
                        a1 += c[(std::size_t) i] * k1[k];
                    }
                }
            y[(std::size_t) j] = a0 + (a1 - a0) * g;
        }
        out.ch.push_back (std::move (y));
    }
    return out;
}

// Signalsmith Stretch over the whole signal (exact output length, latency compensated by its exact() call).
Planar spectralStretch (const Planar& in, long outFrames, double sampleRate, double blockSeconds, double intervalSeconds)
{
    const auto channels = (int) in.ch.size();
    signalsmith::stretch::SignalsmithStretch<float> stretch;
    stretch.configure (channels, (int) std::lround (sampleRate * blockSeconds), (int) std::lround (sampleRate * intervalSeconds));

    // exact() needs at least one block (plus an interval) of input: short sounds are padded with silence and the
    // result cut to length
    const auto ratio = (double) outFrames / (double) in.frames();
    const auto minInput = (long) (stretch.blockSamples() + 2 * stretch.intervalSamples()) * 2;
    Planar padded;
    const Planar* src = &in;
    auto paddedOut = outFrames;
    if (in.frames() < minInput)
    {
        padded = in;
        for (auto& c : padded.ch)
            c.resize ((std::size_t) minInput, 0.0f);
        paddedOut = (long) std::llround ((double) minInput * ratio);
        src = &padded;
    }

    Planar out;
    out.ch.assign ((std::size_t) channels, std::vector<float> ((std::size_t) paddedOut, 0.0f));
    std::vector<const float*> inputs;
    std::vector<float*> outputs;
    for (int c = 0; c < channels; ++c)
    {
        inputs.push_back (src->ch[(std::size_t) c].data());
        outputs.push_back (out.ch[(std::size_t) c].data());
    }
    stretch.exact (inputs.data(), (int) src->frames(), outputs.data(), (int) paddedOut);

    for (auto& c : out.ch)
        c.resize ((std::size_t) outFrames);
    return out;
}

// 64-sample RMS envelope, one value per 16 frames
struct Envelope
{
    static constexpr int hop = 16, width = 64;
    std::vector<float> rms;

    explicit Envelope (const std::vector<float>& x)
    {
        const auto n = (long) x.size();
        rms.resize ((std::size_t) (n / hop + 1));
        for (std::size_t k = 0; k < rms.size(); ++k)
        {
            const auto from = (long) k * hop - width / 2;
            double e = 0.0;
            for (long i = std::max (0L, from); i < std::min (n, from + width); ++i)
                e += (double) x[(std::size_t) i] * x[(std::size_t) i];
            rms[k] = (float) std::sqrt (e / width);
        }
    }
    float at (long frame) const
    {
        const auto k = std::clamp (frame / hop, 0L, (long) rms.size() - 1);
        return rms[(std::size_t) k];
    }
};

double correlation (const float* a, const float* b, long n)
{
    double ab = 0.0, aa = 0.0, bb = 0.0;
    for (long i = 0; i < n; ++i)
    {
        ab += (double) a[i] * b[i];
        aa += (double) a[i] * a[i];
        bb += (double) b[i] * b[i];
    }
    return aa > 0.0 && bb > 0.0 ? ab / std::sqrt (aa * bb) : 0.0;
}

// Puts each detected attack of the input back, unstretched, over the spectral stretcher's output: the original from a
// few ms before the attack to `hold` after it replaces the stretched signal (which smears attacks over its analysis
// block), with short crossfades placed where both signals correlate best.
void spliceAttacks (Planar& out, const Planar& in, double ratio, double sampleRate, double minRiseDb, double holdSeconds)
{
    const auto mono = monoOf (in);
    const auto attacks = stretchdetail::detectAttacks (mono.data(), (long) mono.size(), sampleRate, minRiseDb);
    if (attacks.empty())
        return;

    const auto n = in.frames();
    const auto m = out.frames();
    const auto fadeIn = std::max (16L, (long) (sampleRate * 0.002));
    const auto fadeOut = std::max (32L, (long) (sampleRate * 0.008));
    const auto pre = (long) (sampleRate * 0.004);
    // the original also replaces up to `preRoll` before the attack (when the previous splice leaves room): the spectral
    // stretcher smears an attack backwards over up to half its block, which a 4 ms pre-roll left audible as pre-echo
    const auto preRoll = (long) (sampleRate * 0.015);
    long lastEnd = 0;
    const auto search = (long) (sampleRate * 0.006);
    const auto holdMax = (long) (sampleRate * holdSeconds);
    const auto outMono = monoOf (out);

    for (std::size_t a = 0; a < attacks.size(); ++a)
    {
        const auto o = attacks[a];
        const auto p = (long) std::llround ((double) o * ratio);
        const auto shift = p - o; // output index = input index + shift inside the spliced region

        // room before the next attack (in output frames) and before the previous splice
        const auto nextOut = a + 1 < attacks.size() ? (long) std::llround ((double) attacks[a + 1] * ratio) : m;
        const auto room = nextOut - p - pre - fadeIn - fadeOut;
        if (room < fadeOut)
            continue;
        const auto hold = std::min (holdMax, room);

        // the end of the unstretched part: where stretched and original agree best (least comb filtering in the fade)
        auto bestEnd = p + hold;
        double bestScore = -2.0;
        for (long e = p + std::max (fadeOut, hold - search); e <= p + hold; e += 8)
        {
            const auto src = e - shift;
            if (e + fadeOut > m || src + fadeOut > n || src < 0)
                break;
            const auto s = correlation (outMono.data() + e, mono.data() + src, fadeOut);
            if (s > bestScore)
            {
                bestScore = s;
                bestEnd = e;
            }
        }

        // fade-in begins here: as early as the pre-roll allows, never before the previous splice's end
        const auto latest = p - pre - fadeIn;
        const auto start = std::max ({ p - preRoll - fadeIn, std::min (lastEnd, latest), shift, 0L });
        if (start > latest)
            continue;
        const auto end = std::min (bestEnd + fadeOut, std::min (m, n + shift));
        lastEnd = end;

        for (std::size_t c = 0; c < out.ch.size(); ++c)
        {
            auto& y = out.ch[c];
            const auto& x = in.ch[c];
            for (long i = start; i < end; ++i)
            {
                double w = 1.0;
                if (i < start + fadeIn)
                    w = 0.5 - 0.5 * std::cos (pi * (double) (i - start) / (double) fadeIn);
                else if (i >= bestEnd)
                    w = 0.5 + 0.5 * std::cos (pi * (double) (i - bestEnd) / (double) fadeOut);
                y[(std::size_t) i] = (float) ((1.0 - w) * y[(std::size_t) i] + w * x[(std::size_t) (i - shift)]);
            }
        }
    }
}
} // namespace

namespace stretchdetail
{
std::vector<long> detectAttacks (const float* mono, long frames, double sampleRate, double minRiseDb)
{
    // An attack = the (high-passed) RMS envelope rising by at least `minRiseDb` within ~3 ms over its level just before, above an
    // absolute floor; at most one per 50 ms (the strongest).
    std::vector<long> found;
    if (frames < 256)
        return found;

    // the envelope of the first difference: attacks carry high frequencies, the low-frequency tail of the previous hit
    // (a kick, a bass) does not mask them
    std::vector<float> slope ((std::size_t) frames);
    slope[0] = mono[0];
    for (long i = 1; i < frames; ++i)
        slope[(std::size_t) i] = mono[i] - mono[i - 1];
    const Envelope env (slope);
    const auto& r = env.rms;
    const auto peak = *std::max_element (r.begin(), r.end());
    if (peak <= 0.0f)
        return found;

    const auto floorLevel = peak * 0.02f; // −34 dB below the loudest moment
    const auto riseFactor = (float) std::pow (10.0, minRiseDb / 20.0);
    const auto ahead = std::max (1L, (long) (sampleRate * 0.003) / Envelope::hop);
    const auto behind = std::max (1L, (long) (sampleRate * 0.010) / Envelope::hop);
    const auto minGap = (long) (sampleRate * 0.05);

    struct Candidate
    {
        long frame;
        float rise;
    };
    std::vector<Candidate> candidates;
    for (long k = behind; k + ahead < (long) r.size(); ++k)
    {
        // level before: the maximum over the previous 10 ms (a decaying tail is not an attack)
        float before = 0.0f;
        for (long j = k - behind; j < k; ++j)
            before = std::max (before, r[(std::size_t) j]);
        const auto after = r[(std::size_t) (k + ahead)];
        if (after < floorLevel)
            continue;
        const auto rise = after / std::max (before, peak * 1e-4f);
        if (rise >= riseFactor)
            candidates.push_back ({ k * Envelope::hop, rise });
    }

    // keep the strongest of each cluster, at least `minGap` apart
    std::sort (candidates.begin(), candidates.end(), [] (const Candidate& a, const Candidate& b) { return a.rise > b.rise; });
    for (const auto& c : candidates)
        if (std::none_of (found.begin(), found.end(), [&] (long f) { return std::abs (f - c.frame) < minGap; }))
            found.push_back (c.frame);
    std::sort (found.begin(), found.end());

    // to the sample: the first 8-frame step whose level reaches half the attack's peak (the time map is exact only at
    // the real start of the attack; the envelope frame can be a few ms early)
    const auto look = (long) (sampleRate * 0.012);
    for (auto& f : found)
    {
        const auto from = std::max (0L, f - (long) (sampleRate * 0.002));
        const auto to = std::min (frames - 8, f + look);
        double peakStep = 0.0;
        for (long i = from; i < to; i += 8)
        {
            double e = 0.0;
            for (long j = i; j < i + 8; ++j)
                e = std::max (e, (double) std::abs (mono[j]));
            peakStep = std::max (peakStep, e);
        }
        for (long i = from; i < to; i += 8)
        {
            double e = 0.0;
            long at = i;
            for (long j = i; j < i + 8; ++j)
                if (std::abs (mono[j]) > e)
                {
                    e = std::abs (mono[j]);
                    at = j;
                }
            if (e >= 0.5 * peakStep)
            {
                f = at;
                break;
            }
        }
    }
    return found;
}

Preset presetFor (StretchMethod method) noexcept
{
    // Measured in round 160 (REA_rodada160 §2) against synthetic signals with a known ideal result: an overlap of 6
    // (interval = block / 6) instead of Signalsmith's default 4 lowers the spectral error on chords and voices by about
    // a third and the tonal noise by 20…40 dB; short blocks keep drums and voices tighter, long ones chords cleaner.
    switch (method)
    {
        case StretchMethod::transients: return { 0.06, 0.01, 6.0, 0.030 };
        case StretchMethod::mono: return { 0.06, 0.01, 9.0, 0.020 };
        case StretchMethod::general: break;
    }
    return { 0.12, 0.02, 9.0, 0.020 };
}

std::shared_ptr<SampleData> stretchWith (const SampleData& in, double ratio, const Preset& preset)
{
    const auto planar = planarOf (in);
    const auto frames = planar.frames();
    const auto outFrames = std::max (1L, (long) std::llround ((double) frames * ratio));
    auto out = spectralStretch (planar, outFrames, in.sampleRate, preset.blockSeconds, preset.intervalSeconds);
    if (preset.attackRiseDb > 0.0)
        spliceAttacks (out, planar, (double) outFrames / (double) frames, in.sampleRate, preset.attackRiseDb, preset.holdSeconds);
    return sampleOf (std::move (out), in);
}
} // namespace stretchdetail

struct VoicePitchShifter::Impl
{
    signalsmith::stretch::SignalsmithStretch<float> stretch { 162L }; // fixed seed: renders repeat exactly
    int prime = 0;
    double factor = 1.0;
};

VoicePitchShifter::VoicePitchShifter() = default;
VoicePitchShifter::~VoicePitchShifter() = default;

void VoicePitchShifter::allocate (double sampleRate, StretchMethod method)
{
    impl = std::make_unique<Impl>();
    // real time: 60 ms blocks, overlap 3 (measured round 162: ~1 ms to prime a voice and ~2 % of a core per voice,
    // against ~8 ms and ~10 % with the offline presets); drums get shorter blocks for tighter attacks
    const auto block = method == StretchMethod::transients ? 0.04 : 0.06;
    impl->stretch.configure (2, (int) std::lround (sampleRate * block), (int) std::lround (sampleRate * block / 3.0));
    impl->prime = impl->stretch.outputSeekLength (1.0f);
    // one silent run sizes every internal buffer, so the audio thread's first note does not allocate
    std::vector<float> silence ((std::size_t) std::max (impl->prime, 256), 0.0f), out (256, 0.0f);
    start (silence.data(), silence.data(), 1.5);
    process (silence.data(), silence.data(), out.data(), out.data() + 128, 128, 1.5);
}

bool VoicePitchShifter::ready() const noexcept { return impl != nullptr; }

int VoicePitchShifter::primeLength() const noexcept { return impl != nullptr ? impl->prime : 0; }

void VoicePitchShifter::start (const float* l, const float* r, double factor) noexcept
{
    if (impl == nullptr)
        return;
    impl->factor = factor;
    impl->stretch.setTransposeFactor ((float) factor);
    const float* in[2] { l, r };
    impl->stretch.outputSeek (in, impl->prime);
}

void VoicePitchShifter::process (const float* inL, const float* inR, float* outL, float* outR, int n, double factor) noexcept
{
    if (impl == nullptr)
        return;
    if (factor != impl->factor)
    {
        impl->factor = factor;
        impl->stretch.setTransposeFactor ((float) factor);
    }
    const float* in[2] { inL, inR };
    float* out[2] { outL, outR };
    impl->stretch.process (in, n, out, n);
}

StretchMethod stretchMethodForFlMode (int flMode) noexcept
{
    switch (flMode)
    {
        case 2:  // E3 mono
        case 8:  // E2 mono
        case 9:  // E2 speech
            return StretchMethod::mono;
        case 3:  // slice stretch
        case 4:  // slice map
        case 7:  // E2 transient
            return StretchMethod::transients;
        default: // Stretch Pro (−2), Stretch (−1), E3 generic (1), Auto (5), E2 generic (6)
            return StretchMethod::general;
    }
}

std::shared_ptr<SampleData> timeStretch (const SampleData& in, double ratio, StretchMethod method)
{
    const auto frames = (long) in.frames();

    if (frames < 4 || ! (ratio > 0.0) || std::abs (ratio - 1.0) < 1e-9)
    {
        auto out = std::make_shared<SampleData>();
        out->sampleRate = in.sampleRate;
        out->left.assign (in.leftData(), in.leftData() + frames);
        if (in.isStereo())
            out->right.assign (in.rightData(), in.rightData() + frames);
        return out;
    }

    ratio = std::clamp (ratio, 0.05, 20.0);
    const auto outFrames = std::max (1L, (long) std::llround ((double) frames * ratio));
    const auto planar = planarOf (in);

    if (std::abs (ratio - 1.0) <= stretchdetail::resampleTolerance)
        return sampleOf (resampleTo (planar, outFrames), in);

    const auto preset = stretchdetail::presetFor (method);
    auto out = spectralStretch (planar, outFrames, in.sampleRate, preset.blockSeconds, preset.intervalSeconds);
    if (preset.attackRiseDb > 0.0)
        spliceAttacks (out, planar, (double) outFrames / (double) frames, in.sampleRate, preset.attackRiseDb, preset.holdSeconds);
    return sampleOf (std::move (out), in);
}
} // namespace lynx
