#include "core/VocalPitch.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace lynx::vocal
{
int experiment = 0;
float experimentRelaxed = 0.35f;
float experimentBridgeMs = 25.0f;
float experimentLock = 0.15f;
float experimentNormLo = 0.5f;

namespace
{
constexpr double pi = 3.14159265358979323846;
}

// ---------------------------------------------------------------------------------------------------------------------
// Scales (the usual Western interval sets, from the key's root)

const char* scaleName (Scale s) noexcept
{
    switch (s)
    {
        case Scale::chromatic: return "Chromatic";
        case Scale::major: return "Major";
        case Scale::minor: return "Minor";
        case Scale::harmonicMinor: return "Harmonic minor";
        case Scale::melodicMinor: return "Melodic minor";
        case Scale::majorPentatonic: return "Major pentatonic";
        case Scale::minorPentatonic: return "Minor pentatonic";
        case Scale::blues: return "Blues";
        case Scale::dorian: return "Dorian";
        case Scale::mixolydian: return "Mixolydian";
        case Scale::phrygian: return "Phrygian";
        case Scale::lydian: return "Lydian";
        case Scale::locrian: return "Locrian";
        case Scale::custom: return "Custom";
        case Scale::wholeTone: return "Whole tone";
        case Scale::diminished: return "Diminished";
    }
    return "Chromatic";
}

std::pair<int, Scale> detectKey (const std::array<double, 12>& h) noexcept
{
    // Krumhansl & Kessler (1982) probe-tone profiles, C major / C minor
    static constexpr double major[12] { 6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88 };
    static constexpr double minor[12] { 6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17 };
    double total = 0.0;
    for (const auto x : h)
        total += std::max (0.0, x);
    if (! (total > 0.0))
        return { -1, Scale::major };
    auto corr = [&h] (const double* prof, int key) {
        double mx = 0.0, mp = 0.0;
        for (int k = 0; k < 12; ++k)
        {
            mx += std::max (0.0, h[(std::size_t) k]);
            mp += prof[k];
        }
        mx /= 12.0;
        mp /= 12.0;
        double sxy = 0.0, sxx = 0.0, syy = 0.0;
        for (int k = 0; k < 12; ++k)
        {
            const auto x = std::max (0.0, h[(std::size_t) ((k + key) % 12)]) - mx, y = prof[k] - mp;
            sxy += x * y;
            sxx += x * x;
            syy += y * y;
        }
        return sxx > 0.0 && syy > 0.0 ? sxy / std::sqrt (sxx * syy) : -2.0;
    };
    int bestKey = 0;
    auto bestScale = Scale::major;
    double best = -3.0;
    for (int key = 0; key < 12; ++key)
        for (const auto mode : { Scale::major, Scale::minor })
            if (const auto c = corr (mode == Scale::major ? major : minor, key); c > best + 1e-12)
            {
                best = c;
                bestKey = key;
                bestScale = mode;
            }
    return { bestKey, bestScale };
}

std::uint16_t scaleMask (Scale s, int key, std::uint16_t customMask) noexcept
{
    // intervals above the root, bit k = k semitones
    auto bits = [] (std::initializer_list<int> steps) {
        std::uint16_t m = 0;
        for (auto k : steps)
            m = (std::uint16_t) (m | (1u << k));
        return m;
    };
    std::uint16_t rel = 0xfff;
    switch (s)
    {
        case Scale::chromatic: return 0xfff;
        case Scale::custom: return (std::uint16_t) (customMask & 0xfff);
        case Scale::major: rel = bits ({ 0, 2, 4, 5, 7, 9, 11 }); break;
        case Scale::minor: rel = bits ({ 0, 2, 3, 5, 7, 8, 10 }); break;
        case Scale::harmonicMinor: rel = bits ({ 0, 2, 3, 5, 7, 8, 11 }); break;
        case Scale::melodicMinor: rel = bits ({ 0, 2, 3, 5, 7, 9, 11 }); break;
        case Scale::majorPentatonic: rel = bits ({ 0, 2, 4, 7, 9 }); break;
        case Scale::minorPentatonic: rel = bits ({ 0, 3, 5, 7, 10 }); break;
        case Scale::blues: rel = bits ({ 0, 3, 5, 6, 7, 10 }); break;
        case Scale::dorian: rel = bits ({ 0, 2, 3, 5, 7, 9, 10 }); break;
        case Scale::mixolydian: rel = bits ({ 0, 2, 4, 5, 7, 9, 10 }); break;
        case Scale::phrygian: rel = bits ({ 0, 1, 3, 5, 7, 8, 10 }); break;
        case Scale::lydian: rel = bits ({ 0, 2, 4, 6, 7, 9, 11 }); break;
        case Scale::locrian: rel = bits ({ 0, 1, 3, 5, 6, 8, 10 }); break;
        case Scale::wholeTone: rel = bits ({ 0, 2, 4, 6, 8, 10 }); break;
        case Scale::diminished: rel = bits ({ 0, 2, 3, 5, 6, 8, 9, 11 }); break; // whole-half
    }
    key = ((key % 12) + 12) % 12;
    std::uint16_t m = 0;
    for (int k = 0; k < 12; ++k)
        if (rel & (1u << k))
            m = (std::uint16_t) (m | (1u << ((k + key) % 12)));
    return m;
}

int nearestNote (double note, std::uint16_t mask, int current, double hysteresis) noexcept
{
    mask &= 0xfff;
    auto allowed = [mask] (int n) { return mask == 0 || (mask & (1u << (((n % 12) + 12) % 12))) != 0; };

    if (current >= 0 && allowed (current) && std::abs (note - current) < 0.5 + hysteresis)
        return current;

    const auto base = (int) std::lround (note);
    int best = base;
    double bestD = std::numeric_limits<double>::max();
    for (int d = -6; d <= 6; ++d)
    {
        const auto n = base + d;
        if (! allowed (n))
            continue;
        const auto dist = std::abs (note - n);
        if (dist < bestD - 1e-12)
        {
            bestD = dist;
            best = n;
        }
    }
    return best;
}

double hzToNote (double hz, double a4) noexcept { return 69.0 + 12.0 * std::log2 (hz / a4); }
double noteToHz (double note, double a4) noexcept { return a4 * std::exp2 ((note - 69.0) / 12.0); }

// ---------------------------------------------------------------------------------------------------------------------
// Pitch detector

PitchDetector::PitchDetector()
    : full ((std::size_t) maxFull, 0.0f), deci ((std::size_t) maxDecimated, 0.0f), diff ((std::size_t) maxDecimated, 0.0f)
{
}

void PitchDetector::prepare (double sampleRate, double fminHz, double fmaxHz, int hop) noexcept
{
    sr = sampleRate > 0.0 ? sampleRate : 44100.0;
    fmin = std::clamp (fminHz, 30.0, 1000.0);
    fmax = std::clamp (fmaxHz, fmin * 2.0, 2000.0);
    dec = std::max (1, (int) std::lround (sr / 11025.0));
    const auto fsd = sr / dec;
    tauMax = std::min ((int) std::ceil (fsd / fmin) + 2, maxDecimated / 2 - 8);
    tauMin = std::max (2, (int) std::floor (fsd / fmax));
    window = tauMax;
    hopDec = std::max (1, (int) std::lround ((double) hop / dec));
    guardMemory = std::max (1, (int) std::lround (0.06 * sr / (hopDec * dec)));
    centre = (window + tauMax) / 2 * dec;

    // 4th-order Butterworth low-pass (two biquads, Q 0.5412 / 1.3066) before decimation; RBJ cookbook form
    const auto fc = std::min (0.35 * fsd, 2000.0);
    const double qs[2] { 0.54119610, 1.30656296 };
    for (int s = 0; s < 2; ++s)
    {
        const auto w = 2.0 * pi * fc / sr;
        const auto cw = std::cos (w), alpha = std::sin (w) / (2.0 * qs[s]);
        const auto a0 = 1.0 + alpha;
        auto& b = lp[(std::size_t) s];
        b.b0 = (float) ((1.0 - cw) * 0.5 / a0);
        b.b1 = (float) ((1.0 - cw) / a0);
        b.b2 = b.b0;
        b.a1 = (float) (-2.0 * cw / a0);
        b.a2 = (float) ((1.0 - alpha) / a0);
    }
    reset();
}

void PitchDetector::reset() noexcept
{
    for (auto& b : lp)
        b.z1 = b.z2 = 0.0f;
    std::fill (full.begin(), full.end(), 0.0f);
    std::fill (deci.begin(), deci.end(), 0.0f);
    written = writtenDec = 0;
    phase = hopCount = 0;
    est = {};
    lastTau = 0.0;
    sinceVoiced = 1 << 20;
}

bool PitchDetector::push (float x) noexcept
{
    auto y = lp[1].run (lp[0].run (x));
    if (! std::isfinite (y))
    {
        reset();
        y = 0.0f;
    }
    full[(std::size_t) (written & (maxFull - 1))] = y;
    ++written;

    if (++phase < dec)
        return false;
    phase = 0;
    deci[(std::size_t) (writtenDec & (maxDecimated - 1))] = y;
    ++writtenDec;

    if (++hopCount < hopDec || writtenDec < window + tauMax + 1)
        return false;
    hopCount = 0;
    analyse();
    return true;
}

void PitchDetector::analyse() noexcept
{
    constexpr auto dm = maxDecimated - 1;
    const auto base = writtenDec - (window + tauMax);
    auto at = [&] (std::int64_t j) { return deci[(std::size_t) ((base + j) & dm)]; };

    // YIN steps 2-3: difference function and its cumulative mean normalised form
    double energy = 0.0;
    for (int j = 0; j < window; ++j)
        energy += (double) at (j) * at (j);
    est.level = (float) std::sqrt (energy / window);

    diff[0] = 1.0f;
    double running = 0.0;
    for (int tau = 1; tau <= tauMax; ++tau)
    {
        double d = 0.0;
        for (int j = 0; j < window; ++j)
        {
            const auto e = (double) at (j) - at (j + tau);
            d += e * e;
        }
        running += d;
        diff[(std::size_t) tau] = running > 0.0 ? (float) (d * tau / running) : 1.0f;
    }

    // step 4: the first dip under the threshold (then down to its local minimum); otherwise the global minimum
    int tau = -1;
    for (int t = tauMin; t < tauMax; ++t)
        if (diff[(std::size_t) t] < threshold)
        {
            while (t + 1 < tauMax && diff[(std::size_t) t + 1] < diff[(std::size_t) t])
                ++t;
            tau = t;
            break;
        }
    bool found = tau > 0;
    // Round 157 (setHarmonicGuard): YIN's first dip under the threshold can be a harmonic's — her takes on open vowels
    // ([EXEC] REA_rodada157c §3: a strong ~870 Hz formant over a 290 Hz note) read 3x, 2x or 1.5x the sung pitch for a
    // few estimates; the shifter's grains then shrank to a third of a period (a metallic, gritty burst). A dip at 1.5,
    // 2, 3 or 4 times that lag that is clearly deeper wins when it continues the last voiced period (or, at a phrase's
    // start, when it is much deeper). Only a real margin counts: on a clean tone every multiple's dip is ~0.
    if (found && harmonicGuard)
    {
        const auto prev = sinceVoiced <= guardMemory ? lastTau : 0.0;
        const auto first = diff[(std::size_t) tau];
        int bestTau = tau;
        float bestV = first;
        for (const auto m : { 1.5, 2.0, 3.0, 4.0 })
        {
            const auto c = (int) std::lround (m * tau);
            if (c + 3 >= tauMax)
                break;
            int t2 = c - 2;
            for (int t = c - 2; t <= c + 2; ++t)
                if (diff[(std::size_t) t] < diff[(std::size_t) t2])
                    t2 = t;
            const auto v = diff[(std::size_t) t2];
            const auto continues = prev > 0.0 && std::abs (t2 - prev) < 0.12 * prev && std::abs (t2 - prev) < std::abs (tau - prev);
            const auto deeper = v < 0.6f * first && first - v >= 0.03f && v < bestV; // margin: a clean tone's multiples dip as deep
            if (deeper && (continues || (prev <= 0.0 && m >= 2.0 && v < 0.35f * first)))
            {
                bestTau = t2;
                bestV = v;
            }
        }
        tau = bestTau;
    }
    // EXPERIMENT E1: voicing hysteresis — no dip under the threshold, but the last estimate was voiced a moment ago
    // and the difference function still dips clearly at (about) that period: the note goes on (a breathy or rough
    // stretch), not an unvoiced gap
    if (! found && harmonicGuard && (experiment & 1) != 0 && sinceVoiced <= guardMemory / 2 && lastTau > 0.0)
    {
        const auto lo = std::max (tauMin + 1, (int) std::floor (lastTau * 0.9)), hi = std::min (tauMax - 2, (int) std::ceil (lastTau * 1.1));
        int t2 = -1;
        for (int t = lo; t <= hi; ++t)
            if (diff[(std::size_t) t] <= diff[(std::size_t) t - 1] && diff[(std::size_t) t] <= diff[(std::size_t) t + 1] && (t2 < 0 || diff[(std::size_t) t] < diff[(std::size_t) t2]))
                t2 = t;
        if (t2 > 0 && diff[(std::size_t) t2] < experimentRelaxed)
        {
            tau = t2;
            found = true;
        }
    }
    if (! found)
    {
        tau = tauMin;
        for (int t = tauMin + 1; t < tauMax; ++t)
            if (diff[(std::size_t) t] < diff[(std::size_t) tau])
                tau = t;
    }
    est.clarity = std::clamp (1.0f - diff[(std::size_t) tau], 0.0f, 1.0f);
    est.voiced = found && est.level > gate && tau > tauMin && tau < tauMax - 1;
    if (est.voiced)
    {
        lastTau = tau;
        sinceVoiced = 0;
    }
    else if (sinceVoiced < 1 << 20)
        ++sinceVoiced;
    if (! est.voiced)
        return; // keep the last period (the shifter keeps its marks' spacing through short gaps)

    // step 5: parabolic interpolation
    auto refine = [] (double a, double b, double c) {
        const auto den = a - 2.0 * b + c;
        return std::abs (den) > 1e-12 ? std::clamp (0.5 * (a - c) / den, -0.5, 0.5) : 0.0;
    };
    auto period = ((double) tau + refine (diff[(std::size_t) tau - 1], diff[(std::size_t) tau], diff[(std::size_t) tau + 1])) * dec;

    // full-rate refinement: the raw difference function around the coarse period
    if (dec > 1)
    {
        constexpr auto fm = maxFull - 1;
        const auto lo = std::max (2, (int) std::floor (period) - dec - 1);
        const auto hi = (int) std::ceil (period) + dec + 1;
        const auto wf = std::min (window * dec, 2048);
        if (wf + hi + 4 < maxFull && written > wf + hi + centre + 1)
        {
            // centred like the YIN window (so a gliding pitch is read at the same instant)
            const auto start = std::min (written - centre - (wf + (std::int64_t) period) / 2, written - wf - hi - 1);
            auto y = [&] (std::int64_t j) { return full[(std::size_t) ((start + j) & fm)]; };
            std::array<double, 64> d {};
            const auto count = std::min (hi - lo + 1, 64);
            int bestK = 0;
            for (int k = 0; k < count; ++k)
            {
                double s = 0.0;
                for (int j = 0; j < wf; ++j)
                {
                    const auto e = (double) y (j) - y (j + lo + k);
                    s += e * e;
                }
                d[(std::size_t) k] = s;
                if (s < d[(std::size_t) bestK])
                    bestK = k;
            }
            if (bestK > 0 && bestK < count - 1)
                period = lo + bestK + refine (d[(std::size_t) bestK - 1], d[(std::size_t) bestK], d[(std::size_t) bestK + 1]);
        }
    }

    est.period = period;
    est.hz = sr / period;
}

std::vector<PitchFrame> analysePitch (const float* x, int n, double sampleRate, int hop, double fminHz, double fmaxHz, float threshold)
{
    hop = std::max (1, hop);
    std::vector<PitchFrame> frames ((std::size_t) (n / hop + 1));
    if (n <= 0 || x == nullptr)
        return frames;

    PitchDetector det;
    det.prepare (sampleRate, fminHz, fmaxHz, std::max (1, hop / 2));
    det.setThreshold (threshold);
    const auto c = det.windowCentre();

    // estimates at their window centres
    struct At
    {
        std::int64_t t;
        PitchEstimate e;
    };
    std::vector<At> all;
    all.reserve ((std::size_t) (2 * n / std::max (1, hop / 2) + 4));
    for (int i = 0; i < n + c; ++i)
        if (det.push (i < n ? x[i] : 0.0f))
            all.push_back ({ (std::int64_t) i - c, det.estimate() });

    std::size_t k = 0;
    for (std::size_t f = 0; f < frames.size(); ++f)
    {
        const auto t = (std::int64_t) f * hop;
        while (k + 1 < all.size() && std::llabs (all[k + 1].t - t) <= std::llabs (all[k].t - t))
            ++k;
        if (all.empty())
            break;
        const auto& e = all[k].e;
        frames[f].hz = e.voiced ? e.hz : 0.0;
        frames[f].clarity = e.clarity;
        frames[f].level = e.level;
    }
    return frames;
}

// ---------------------------------------------------------------------------------------------------------------------
// TD-PSOLA

Psola::Psola() : env ((std::size_t) ringSize, 0.0f), hfEnv ((std::size_t) ringSize, 0.0f), wsum ((std::size_t) ringSize, 0.0f)
{
    for (int c = 0; c < 2; ++c)
    {
        in[c].assign ((std::size_t) ringSize, 0.0f);
        acc[c].assign ((std::size_t) ringSize, 0.0f);
    }
}

void Psola::prepare (double sampleRate, double maxPeriod, int latency) noexcept
{
    sr = sampleRate > 0.0 ? sampleRate : 44100.0;
    maxP = std::clamp (maxPeriod, 16.0, ringSize / 8.0);
    delay = std::clamp (latency, 8, ringSize / 2);
    lag = 0;
    unvoicedHop = std::clamp ((int) std::lround (128.0 * sr / 44100.0), 4, std::max (4, (delay - 4) / 2));
    reset();
}

void Psola::reset() noexcept
{
    for (int c = 0; c < 2; ++c)
    {
        std::fill (in[c].begin(), in[c].end(), 0.0f);
        std::fill (acc[c].begin(), acc[c].end(), 0.0f);
    }
    std::fill (env.begin(), env.end(), 0.0f);
    std::fill (hfEnv.begin(), hfEnv.end(), 0.0f);
    std::fill (wsum.begin(), wsum.end(), 0.0f);
    envZ = hpZ = hfZ = 0.0f;
    hfLead = (int) std::lround (sr / (2.0 * pi * 160.0));
    now = 0;
    nextMark = 0.0;
    nextSynth = 0.0;
    markHead = markCount = 0;
    period = 0.0;
    voiced = false;
    lastVoiced = false;
    lastMark = 0.0;
    voicedAt = -(1 << 30);
    ratio = formant = 1.0;
}

void Psola::setMarkLag (int samples) noexcept
{
    lag = std::clamp (samples, 0, std::max (0, delay - (int) std::ceil (1.5 * maxP)));
}

void Psola::setPitch (double p, bool v) noexcept
{
    voiced = v && p >= 2.0 && p <= maxP * 1.5;
    if (voiced)
    {
        period = p;
        voicedAt = now;
    }
}

void Psola::setShift (double r, double f) noexcept
{
    ratio = std::clamp (std::isfinite (r) ? r : 1.0, 0.25, 4.0);
    formant = std::clamp (std::isfinite (f) ? f : 1.0, 0.5, 2.0);
}

float Psola::readIn (int ch, double pos) const noexcept
{
    const auto i = (std::int64_t) std::floor (pos);
    if (i - 1 < now - ringSize + 2 || i + 2 > now - 1 || i - 1 < 0)
        return 0.0f;
    const auto t = (float) (pos - (double) i);
    constexpr auto m = ringSize - 1;
    const auto& b = in[ch];
    const auto y0 = b[(std::size_t) ((i - 1) & m)], y1 = b[(std::size_t) (i & m)], y2 = b[(std::size_t) ((i + 1) & m)],
               y3 = b[(std::size_t) ((i + 2) & m)];
    // 4-point Hermite
    const auto c1 = 0.5f * (y2 - y0);
    const auto c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    const auto c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * t + c2) * t + c1) * t + y1;
}

void Psola::placeMarks() noexcept
{
    const auto last = (double) (now - 1 - lag);
    // far behind (start, or after a long period): restart the marks at the present
    if (nextMark < last - 2.0 * maxP)
        nextMark = last;

    auto push = [this] (double pos, float p, bool v) {
        if (markCount == maxMarks)
        {
            markHead = (markHead + 1) % maxMarks;
            --markCount;
        }
        marks[(std::size_t) ((markHead + markCount) % maxMarks)] = { pos, p, v, (float) ratio, (float) formant };
        ++markCount;
    };

    // EXPERIMENT E2: a short voicing dropout (under bridgeMs) keeps pitch-synchronous marks at the last period
    const bool bridged = ! voiced && (experiment & 2) != 0 && period > 0.0 && lastMark > 0.0 && (double) (now - voicedAt) < experimentBridgeMs * 0.001 * sr;
    const bool asVoiced = (voiced || bridged) && period > 0.0;
    if (! asVoiced)
    {
        if (nextMark <= last)
        {
            push (nextMark, (float) unvoicedHop, false);
            nextMark += unvoicedHop;
        }
    }
    else if (peakAlign)
    {
        // EXPERIMENT E3: a phrase's first mark (no previous mark a period back) searches the whole period
        const bool chain = lastMark > 0.0 && std::abs ((nextMark - lastMark) - period) < 0.5 * period;
        const auto reach = period * ((experiment & 4) != 0 && ! chain ? 0.5 : 0.25);
        if (nextMark + reach <= last)
        {
            constexpr auto m = ringSize - 1;
            const auto from = std::max<std::int64_t> ((std::int64_t) std::ceil (nextMark - reach), now - ringSize + 1),
                       to = (std::int64_t) std::floor (nextMark + reach);
            // Round 148: the mark goes where the waveform repeats the one at the previous mark best (the cross-correlation
            // of the low-passed signal over one period, peak refined to a fraction of a sample), not to the highest
            // single peak. [EXEC] (REA_rodada148 §1) the peak in noise moved by a sample or two from period to period:
            // the held note wobbled 4–11 c on low notes with white noise at 20 / 10 dB SNR or breath noise (Auto-Tune
            // Pro: 0.3–4 c); a whole period integrates the noise away, and the fraction of a sample matters on high
            // notes (the integer marks alone left 2 c of jitter). The first mark of a phrase still takes the peak.
            const auto prev = lastMark;
            const auto gap = (double) from - prev;
            const bool corr = prev > 0.0 && gap > 0.5 * period && gap < 1.5 * period && prev - 2.0 * period > (double) (now - ringSize + 2);
            // the period just before each (it is in even with the shortest latency)
            const auto span = (int) period;
            const auto pm = (std::int64_t) std::lround (prev);
            // long periods: every other sample (or fewer) of the window — the low-passed signal is smooth there
            const auto stride = std::max (1, span / 256);
            auto score = [&] (std::int64_t i) {
                if (! corr)
                    return (experiment & 4) != 0 ? (double) hfEnv[(std::size_t) ((i + hfLead) & m)] : (double) env[(std::size_t) (i & m)];
                double sxy = 0.0;
                for (int j = -span; j <= 0; j += stride)
                    sxy += (double) env[(std::size_t) ((i + j) & m)] * (double) env[(std::size_t) ((pm + j) & m)];
                return sxy;
            };
            // prefer the predicted position a little: ties and flat tops do not make the marks wander
            const auto pen = corr ? 0.0 : 1e-4;
            std::int64_t best = std::clamp<std::int64_t> ((std::int64_t) std::lround (nextMark), from, std::max (from, to));
            auto top = -std::numeric_limits<double>::max();
            for (auto i = from; i <= to; ++i)
            {
                const auto v = score (i) - pen * std::abs ((double) i - nextMark) / period;
                if (v > top)
                {
                    top = v;
                    best = i;
                }
            }
            auto pos = (double) best;
            if (best > from && best < to)
            {
                const auto y0 = score (best - 1), y1 = score (best), y2 = score (best + 1), den = y0 - 2.0 * y1 + y2;
                if (den < 0.0)
                    pos += std::clamp (0.5 * (y0 - y2) / den, -0.5, 0.5);
            }
            if (corr)
                pos += prev - (double) pm; // the lag is measured from the previous mark's own (fractional) position
            // EXPERIMENT E5: a slow phase lock of the tracked marks onto the high-frequency energy peak (the pulse)
            if (corr && (experiment & 8) != 0)
            {
                const auto c0 = (std::int64_t) std::lround (pos);
                const auto w = (std::int64_t) std::max (1.0, period * 0.25);
                auto bi = c0;
                for (auto i = c0 - w; i <= c0 + w; ++i)
                    if (i + hfLead <= now - 1 && hfEnv[(std::size_t) ((i + hfLead) & m)] > hfEnv[(std::size_t) ((bi + hfLead) & m)])
                        bi = i;
                pos += experimentLock * (double) (bi - c0);
            }
            push (pos, (float) period, true);
            lastMark = pos;
            nextMark = pos + period;
        }
    }
    else if (nextMark <= last)
    {
        push (nextMark, (float) period, true);
        nextMark += period;
    }

    // forget the marks no grain can use any more
    const auto oldest = (double) (now - 1 - delay) - 2.0 * maxP - 8.0;
    while (markCount > 1 && marks[(std::size_t) markHead].pos < oldest)
    {
        markHead = (markHead + 1) % maxMarks;
        --markCount;
    }
}

void Psola::process (const float* x, float* y, int channels) noexcept
{
    constexpr auto m = ringSize - 1;
    channels = std::clamp (channels, 1, 2);
    for (int c = 0; c < channels; ++c)
        in[c][(std::size_t) (now & m)] = std::isfinite (x[c]) ? x[c] : 0.0f;
    const auto mono = channels == 2 ? 0.5f * (in[0][(std::size_t) (now & m)] + in[1][(std::size_t) (now & m)]) : in[0][(std::size_t) (now & m)];
    // a gentle low-pass (one pole near 900 Hz) of the input: the marks lock to its peaks
    const auto a = (float) (1.0 - std::exp (-2.0 * pi * 900.0 / sr));
    envZ += a * (mono - envZ);
    if (! std::isfinite (envZ))
        envZ = 0.0f;
    env[(std::size_t) (now & m)] = envZ;
    // EXPERIMENT E3: the high-frequency energy envelope (above ~1.5 kHz, smoothed ~1 ms): its peak is the glottal pulse
    {
        const auto ah = (float) (1.0 - std::exp (-2.0 * pi * 1500.0 / sr)), ae = (float) (1.0 - std::exp (-2.0 * pi * 160.0 / sr));
        hpZ += ah * (mono - hpZ);
        const auto h = mono - hpZ;
        hfZ += ae * (h * h - hfZ);
        if (! std::isfinite (hfZ) || ! std::isfinite (hpZ)) hfZ = hpZ = 0.0f;
        hfEnv[(std::size_t) (now & m)] = hfZ;
    }
    ++now;

    placeMarks();

    const auto e = now - 1 - delay; // the output sample of this call (input timeline)
    const auto last = (double) (now - 1);

    // overlap-add every grain that starts at or before e
    for (int guard = 0; guard < 64; ++guard)
    {
        if (nextSynth < (double) e - maxP)
            nextSynth = (double) e; // fell behind (start): resume at the present
        if (nextSynth - 2.5 * maxP > (double) e)
            break; // no grain can reach e yet

        // the analysis mark nearest the synthesis mark among those whose whole grain is already in
        int best = -1;
        double bestD = std::numeric_limits<double>::max();
        for (int k = 0; k < markCount; ++k)
        {
            const auto& mk = marks[(std::size_t) ((markHead + k) % maxMarks)];
            const auto need = mk.voiced ? mk.pos + mk.period + 2.0 : mk.pos;
            if (need > last)
                continue;
            const auto d = std::abs (mk.pos - nextSynth);
            if (d < bestD)
            {
                bestD = d;
                best = k;
            }
        }

        const Mark* mk = best >= 0 ? &marks[(std::size_t) ((markHead + best) % maxMarks)] : nullptr;
        const auto isVoiced = mk != nullptr && mk->voiced;
        const auto f = isVoiced ? (double) mk->formant : 1.0;
        const auto r = isVoiced ? (double) mk->ratio : 1.0;
        const auto ha = isVoiced ? (double) mk->period : (double) unvoicedHop; // analysis half-length
        const auto hs = ha / f;                                                // synthesis half-length
        // a voiced onset starts on its mark: the voice is not displaced in time from the input
        if (isVoiced && ! lastVoiced && mk->pos - hs > (double) e - 1.0)
            nextSynth = mk->pos;
        const auto ts = nextSynth;

        if (ts - hs > (double) e)
            break; // this grain starts later (a voiced onset may have moved it ahead)

        // unvoiced: read around the synthesis time itself (plain overlap-add, transparent); voiced: around the mark
        const auto ta = isVoiced ? mk->pos : ts;
        if (! isVoiced && ts + hs + 2.0 > last)
            break; // not in yet (only with a very short latency)

        const auto gain = (float) std::clamp (f / r, 0.25, 4.0);
        const auto o0 = std::max<std::int64_t> ((std::int64_t) std::ceil (ts - hs), e);
        const auto o1 = std::min<std::int64_t> ((std::int64_t) std::floor (ts + hs), e + ringSize - 1);
        const auto invHs = 1.0 / hs;
        for (auto o = o0; o <= o1; ++o)
        {
            const auto u = ((double) o - ts) * invHs;
            if (u <= -1.0 || u >= 1.0)
                continue;
            const auto w = gain * (float) (0.5 + 0.5 * std::cos (pi * u));
            const auto src = ta + ((double) o - ts) * f;
            for (int c = 0; c < channels; ++c)
                acc[c][(std::size_t) (o & m)] += w * readIn (c, src);
            wsum[(std::size_t) (o & m)] += w; // EXPERIMENT E6
        }
        // unshifted voiced grains drift toward their marks, so a voice already on its note comes out as the input
        // itself (delayed), not displaced by up to half a period. Round 148: faded in over the last 3.4 cents of
        // shift and at most 0.05 % of a period per grain (~0.9 c) — the on / off switch at 1.7 cents (0.15 %) turned
        // into a 0.5 c wobble on held notes whose correction hovers around zero
        auto hop = isVoiced ? ha / r : ha;
        if (isVoiced && std::abs (f - 1.0) < 1e-3)
        {
            const auto w = std::max (0.0, 1.0 - std::abs (r - 1.0) / 2e-3);
            if (w > 0.0)
                hop += w * std::clamp (0.5 * (ta - ts), -0.0005 * ha, 0.0005 * ha);
        }
        nextSynth = ts + hop;
        lastVoiced = isVoiced;
    }

    for (int c = 0; c < channels; ++c)
    {
        if (e < 0)
        {
            y[c] = 0.0f;
            continue;
        }
        auto& slot = acc[c][(std::size_t) (e & m)];
        y[c] = slot;
        slot = 0.0f;
    }
    // EXPERIMENT E6: the overlap-add normalised by the summed windows (grains of uneven length / spacing)
    if (e >= 0)
    {
        auto& ws = wsum[(std::size_t) (e & m)];
        if ((experiment & 16) != 0 && ws > 1e-3f)
        {
            const auto g = 1.0f / std::clamp (ws, experimentNormLo, 2.0f);
            for (int c = 0; c < channels; ++c)
                y[c] *= g;
        }
        ws = 0.0f;
    }
}
} // namespace lynx::vocal
