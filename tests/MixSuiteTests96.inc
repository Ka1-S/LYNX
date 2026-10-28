// REA_rodada96: LYNX's own mixing suite (core/MixSuite.h) — measurable responses: EQ curves, gain-reduction curves,
// release behaviour, THD and aliasing, latency.

namespace mixsuite96
{
using lynx::EffectKind;
namespace ms = lynx::mixsuite;

inline lynx::EffectParams defaults (EffectKind kind)
{
    return lynx::makeEffectSlot (kind).params;
}

inline std::vector<float> sine (double hz, double amp, int n, double sr = 44100.0)
{
    std::vector<float> s ((std::size_t) n);
    for (int i = 0; i < n; ++i)
        s[(std::size_t) i] = (float) (amp * std::sin (2.0 * 3.141592653589793 * hz * i / sr));
    return s;
}

// runs the effect on a dual-mono copy of `in`; returns the left output
inline std::vector<float> run (EffectKind kind, const lynx::EffectParams& p, std::vector<float> in, double sr = 44100.0, int block = 512,
                               std::unique_ptr<lynx::Effect>* keep = nullptr)
{
    auto fx = lynx::makeEffect (kind);
    fx->prepare (sr);
    auto r = in;
    for (std::size_t off = 0; off < in.size(); off += (std::size_t) block)
    {
        const auto m = (int) std::min<std::size_t> ((std::size_t) block, in.size() - off);
        fx->process (in.data() + off, r.data() + off, m, p, 120.0);
    }
    if (keep != nullptr)
        *keep = std::move (fx);
    return in;
}

// amplitude of the `hz` component over [start, start + len) (a whole number of cycles is not required: Hann window)
inline double amplitude (const std::vector<float>& x, double hz, std::size_t start, std::size_t len, double sr = 44100.0)
{
    double re = 0.0, im = 0.0, wsum = 0.0;
    for (std::size_t i = 0; i < len; ++i)
    {
        const auto w = 0.5 - 0.5 * std::cos (2.0 * 3.141592653589793 * (double) i / (double) len);
        const auto ph = 2.0 * 3.141592653589793 * hz * (double) (start + i) / sr;
        re += w * x[start + i] * std::cos (ph);
        im += w * x[start + i] * std::sin (ph);
        wsum += w;
    }
    return 2.0 * std::sqrt (re * re + im * im) / wsum;
}

inline double db (double a) { return 20.0 * std::log10 (std::max (a, 1e-15)); }
} // namespace mixsuite96

TEST (mixsuite96_oversampler_latency_and_identity)
{
    using namespace mixsuite96;
    const auto latency = ms::Oversampler::latency();
    std::printf ("  oversampler latency %d samples\n", latency);
    CHECK (latency > 0 && latency < 64);
    for (int factor : { 2, 4, 8 })
    {
        ms::Oversampler os;
        os.setFactor (factor);
        CHECK (os.factor() == factor);
        std::vector<float> x (4096, 0.0f);
        x[100] = 1.0f;
        os.process (x.data(), (int) x.size(), [] (float v) { return v; });
        const auto peak = std::max_element (x.begin(), x.end(), [] (float a, float b) { return std::abs (a) < std::abs (b); }) - x.begin();
        CHECK (peak == 100 + latency);
        CHECK (x[(std::size_t) peak] > 0.8f && x[(std::size_t) peak] < 1.0f); // a band-limited impulse

        // a 1 kHz sine comes back delayed by exactly the latency (passband ripple only)
        auto s = sine (1000.0, 0.5, 8192);
        auto y = s;
        ms::Oversampler os2;
        os2.setFactor (factor);
        os2.process (y.data(), (int) y.size(), [] (float v) { return v; });
        double err = 0.0;
        for (std::size_t i = 1000; i < y.size(); ++i)
            err = std::max (err, (double) std::abs (y[i] - s[i - (std::size_t) latency]));
        std::printf ("  %dx: identity error %.1f dB\n", factor, db (err / 0.5));
        CHECK (db (err / 0.5) < -60.0);
    }
}

TEST (mixsuite96_ridge_eq_curves)
{
    using namespace mixsuite96;
    using namespace lynx::mixsuite::ridge;
    auto p = defaults (EffectKind::ridgeStrip);
    CHECK (std::abs (responseDb (p, 44100.0, 1000.0)) < 0.01);

    // LMF bell: +12 dB at 1 kHz (brown: ±15 dB range)
    p[lmfGain] = (float) ((12.0 / 15.0 + 1.0) / 2.0);
    p[lmfFreq] = (float) (std::log (1000.0 / 200.0) / std::log (12.5));
    const auto brownPeak = responseDb (p, 44100.0, 1000.0), brownOct = responseDb (p, 44100.0, 2000.0);
    std::printf ("  brown LMF: %.2f dB at 1 kHz, %.2f dB an octave up\n", brownPeak, brownOct);
    CHECK (std::abs (brownPeak - 12.0) < 0.05);

    // black: the same knob is +14.4 dB and the bell narrows with the gain (proportional Q)
    p[curve] = 1.0f;
    const auto blackPeak = responseDb (p, 44100.0, 1000.0), blackOct = responseDb (p, 44100.0, 2000.0);
    std::printf ("  black LMF: %.2f dB at 1 kHz, %.2f dB an octave up\n", blackPeak, blackOct);
    CHECK (std::abs (blackPeak - 14.4) < 0.05);
    CHECK (blackPeak - blackOct > brownPeak - brownOct + 1.0);
    // ... and is wider than brown for small gains
    p[lmfGain] = (float) ((2.0 / 18.0 + 1.0) / 2.0);
    const auto blackSmall = responseDb (p, 44100.0, 2000.0) / responseDb (p, 44100.0, 1000.0);
    p[curve] = 0.0f;
    p[lmfGain] = (float) ((2.0 / 15.0 + 1.0) / 2.0);
    const auto brownSmall = responseDb (p, 44100.0, 2000.0) / responseDb (p, 44100.0, 1000.0);
    CHECK (blackSmall > brownSmall);

    // high-pass: 3rd-order Butterworth, 18 dB/oct (−3 dB at the corner, −18.1 dB an octave below)
    p = defaults (EffectKind::ridgeStrip);
    p[hpf] = (float) (std::log (100.0 / 16.0) / std::log (350.0 / 16.0));
    std::printf ("  HPF 100 Hz: %.2f dB at 100 Hz, %.2f at 50 Hz, %.2f at 25 Hz\n", responseDb (p, 44100.0, 100.0),
                 responseDb (p, 44100.0, 50.0), responseDb (p, 44100.0, 25.0));
    CHECK (std::abs (responseDb (p, 44100.0, 100.0) + 3.01) < 0.1);
    CHECK (std::abs (responseDb (p, 44100.0, 50.0) + 18.13) < 0.3);
    CHECK (std::abs (responseDb (p, 44100.0, 25.0) - responseDb (p, 44100.0, 50.0) + 18.0) < 0.5);

    // the processed signal follows the computed curve (HF shelf +9 dB at 8 kHz, tone at 12 kHz)
    p = defaults (EffectKind::ridgeStrip);
    p[hfGain] = (float) ((9.0 / 15.0 + 1.0) / 2.0);
    const auto expect = responseDb (p, 44100.0, 12000.0);
    const auto out = run (EffectKind::ridgeStrip, p, sine (12000.0, 0.1, 22050));
    const auto got = db (amplitude (out, 12000.0, 11025, 8192) / 0.1);
    std::printf ("  HF shelf at 12 kHz: computed %.2f dB, processed %.2f dB\n", expect, got);
    CHECK (expect > 8.0 && std::abs (got - expect) < 0.1);
}

TEST (mixsuite96_ridge_dynamics_curves)
{
    using namespace mixsuite96;
    using namespace lynx::mixsuite::ridge;
    auto p = defaults (EffectKind::ridgeStrip);
    p[compThreshold] = 0.5f;                         // −20 dBFS
    p[compRatio] = (float) std::sqrt (3.0 / 19.0);   // 4:1
    CHECK (std::abs (ratio (p[compRatio]) - 4.0) < 1e-6);
    // static curve: 4:1 above the knee
    CHECK (std::abs (compressorCurveDb (p, 0.0) + 15.0) < 1e-4);
    CHECK (std::abs (compressorCurveDb (p, -40.0) + 40.0) < 1e-4);

    // measured gain reduction curve (steady 1 kHz sines; the detector reads the peak)
    std::vector<double> outDb;
    for (double in : { -40.0, -30.0, -20.0, -10.0, 0.0 })
    {
        const auto amp = std::pow (10.0, in / 20.0);
        const auto y = run (EffectKind::ridgeStrip, p, sine (1000.0, amp, 44100));
        outDb.push_back (db (amplitude (y, 1000.0, 30000, 8192)));
        std::printf ("  in %+.0f dB -> out %+.2f dB (static curve %+.2f)\n", in, outDb.back(), compressorCurveDb (p, in));
        CHECK (std::abs (outDb.back() - compressorCurveDb (p, in)) < 0.6);
    }
    CHECK (std::abs ((outDb[4] - outDb[3]) - 2.5) < 0.3); // 10 dB in → 2.5 dB out

    // gate: 60 dB range, threshold −40 dBFS
    p = defaults (EffectKind::ridgeStrip);
    p[gateRange] = 1.0f;
    p[gateThreshold] = 0.5f;
    const auto quiet = run (EffectKind::ridgeStrip, p, sine (1000.0, std::pow (10.0, -50.0 / 20.0), 44100));
    const auto loud = run (EffectKind::ridgeStrip, p, sine (1000.0, std::pow (10.0, -30.0 / 20.0), 44100));
    std::printf ("  gate: -50 dB tone -> %.1f dB, -30 dB tone -> %.2f dB\n", db (amplitude (quiet, 1000.0, 30000, 8192)),
                 db (amplitude (loud, 1000.0, 30000, 8192)));
    CHECK (db (amplitude (quiet, 1000.0, 30000, 8192)) < -100.0);
    CHECK (std::abs (db (amplitude (loud, 1000.0, 30000, 8192)) + 30.0) < 0.1);
    // expander 1:2: 10 dB under the threshold comes out 20 dB under
    p[expander] = 1.0f;
    const auto exp = run (EffectKind::ridgeStrip, p, sine (1000.0, std::pow (10.0, -50.0 / 20.0), 44100));
    std::printf ("  expander: -50 dB tone -> %.1f dB\n", db (amplitude (exp, 1000.0, 30000, 8192)));
    CHECK (std::abs (db (amplitude (exp, 1000.0, 30000, 8192)) + 60.0) < 1.5);

    // the meters: compressor gain reduction while compressing
    p = defaults (EffectKind::ridgeStrip);
    p[compThreshold] = 0.5f;
    p[compRatio] = 1.0f;
    std::unique_ptr<lynx::Effect> fx;
    run (EffectKind::ridgeStrip, p, sine (1000.0, 1.0, 22050), 44100.0, 512, &fx);
    std::printf ("  meter: GR %.1f dB\n", fx->controlOutput (0) * 30.0f);
    CHECK (std::abs (fx->controlOutput (0) * 30.0f - 20.0f) < 1.5f);
}

TEST (mixsuite96_glow_leveler)
{
    using namespace mixsuite96;
    using namespace lynx::mixsuite::glow;
    // static curve: compress is gentle (about 2:1 to 3:1 at the top), limit is much firmer
    auto slope = [] (float limit) {
        auto p = defaults (EffectKind::glowLeveler);
        p[voicing] = 0.0f; p[gain] = 0.0f; p[emphasis] = 0.0f; // Glow's own (Modern) engine; round 152's defaults are the classic voicing
        p[peakReduction] = 0.6f;
        p[mode] = limit;
        const auto a = db (amplitude (run (EffectKind::glowLeveler, p, sine (1000.0, std::pow (10.0, -12.0 / 20.0), 88200)), 1000.0, 70000, 8192));
        const auto b = db (amplitude (run (EffectKind::glowLeveler, p, sine (1000.0, 1.0, 88200)), 1000.0, 70000, 8192));
        std::printf ("  %s: -12 dB -> %.2f, 0 dB -> %.2f (ratio %.2f)\n", limit > 0.5f ? "limit" : "compress", a, b, 12.0 / (b - a));
        return 12.0 / (b - a);
    };
    const auto compress = slope (0.0f), limit = slope (1.0f);
    CHECK (compress > 1.5 && compress < 3.5);
    CHECK (limit > 5.0);

    // two-stage, program-dependent release: read the gain-reduction meter after a burst
    auto releaseAfter = [] (double burstSeconds) {
        auto p = defaults (EffectKind::glowLeveler);
        p[voicing] = 0.0f; p[gain] = 0.0f; p[emphasis] = 0.0f; // the Modern engine (see above)
        p[peakReduction] = 0.7f;
        auto fx = lynx::makeEffect (EffectKind::glowLeveler);
        fx->prepare (44100.0);
        const int block = 64;
        auto burst = sine (1000.0, 0.7, (int) (burstSeconds * 44100.0));
        std::vector<float> r (burst.size());
        float gr0 = 0.0f;
        for (std::size_t off = 0; off + block <= burst.size(); off += block)
        {
            r.assign (burst.begin() + (std::ptrdiff_t) off, burst.begin() + (std::ptrdiff_t) off + block);
            auto l = r;
            fx->process (l.data(), r.data(), block, p, 120.0);
            gr0 = fx->controlOutput (0) * 30.0f;
        }
        std::vector<float> silence (block, 0.0f), s2 (block, 0.0f);
        std::vector<float> trace;
        for (int b = 0; b < (int) (2.0 * 44100.0 / block); ++b)
        {
            std::fill (silence.begin(), silence.end(), 0.0f);
            std::fill (s2.begin(), s2.end(), 0.0f);
            fx->process (silence.data(), s2.data(), block, p, 120.0);
            trace.push_back (fx->controlOutput (0) * 30.0f);
        }
        const auto at = [&] (double s) { return trace[(std::size_t) (s * 44100.0 / block)] / gr0; };
        std::printf ("  burst %.1f s: GR %.1f dB, left after 0.1 s %.0f%%, 0.5 s %.0f%%, 1.5 s %.0f%%\n", burstSeconds, gr0, 100 * at (0.1),
                     100 * at (0.5), 100 * at (1.5));
        return std::array<double, 4> { gr0, at (0.1), at (0.5), at (1.5) };
    };
    const auto shortBurst = releaseAfter (0.5), longBurst = releaseAfter (6.0);
    CHECK (shortBurst[0] > 3.0);
    CHECK (shortBurst[1] < 0.7);                 // the fast stage: a good part released within 100 ms
    CHECK (shortBurst[2] > 0.05);                // the slow stage: not all of it within half a second
    CHECK (longBurst[2] > shortBurst[2] * 1.3);  // longer leveling, slower release
    CHECK (longBurst[3] < longBurst[2]);
}

TEST (mixsuite96_hush_deesser)
{
    using namespace mixsuite96;
    using namespace lynx::mixsuite::hush;
    auto p = defaults (EffectKind::hushDeEsser);
    p[threshold] = (float) (20.0 / 60.0);     // −40 dBFS
    p[range] = 0.5f;                          // 12 dB
    p[frequency] = 0.5f * (float) (std::log (2.0) / std::log (8.0)) + 0.0f; // 2.83 kHz split
    const auto n = 44100;
    auto mixSig = sine (300.0, 0.3, n);
    const auto ess = sine (7000.0, 0.3, n);
    for (int i = 0; i < n; ++i)
        mixSig[(std::size_t) i] += ess[(std::size_t) i];

    std::unique_ptr<lynx::Effect> fx;
    const auto split = run (EffectKind::hushDeEsser, p, mixSig, 44100.0, 512, &fx);
    const auto lat = fx->latencySamples();
    CHECK (lat == (int) std::lround (lookaheadMs() * 44.1));
    const auto lowSplit = db (amplitude (split, 300.0, 22050, 16384) / 0.3), highSplit = db (amplitude (split, 7000.0, 22050, 16384) / 0.3);
    std::printf ("  split: 300 Hz %+.2f dB, 7 kHz %+.2f dB, GR meter %.1f dB\n", lowSplit, highSplit, fx->controlOutput (0) * 30.0f);
    CHECK (std::abs (lowSplit) < 0.6);
    CHECK (highSplit < -9.0 && highSplit > -18.0);

    p[mode] = 1.0f; // wideband: everything ducks while the sibilance is there
    const auto wide = run (EffectKind::hushDeEsser, p, mixSig);
    const auto lowWide = db (amplitude (wide, 300.0, 22050, 16384) / 0.3);
    std::printf ("  wide: 300 Hz %+.2f dB\n", lowWide);
    CHECK (lowWide < -9.0);

    // a dull signal passes untouched (delayed by the lookahead)
    p[mode] = 0.0f;
    const auto dull = run (EffectKind::hushDeEsser, p, sine (300.0, 0.3, n));
    double err = 0.0;
    const auto ref = sine (300.0, 0.3, n);
    for (int i = 30000; i < n; ++i)
        err = std::max (err, (double) std::abs (dull[(std::size_t) i] - ref[(std::size_t) (i - lat)]));
    std::printf ("  dull signal: error %.2g\n", err);
    CHECK (err < 1e-4);

    // listen: the detector band only
    p[listen] = 1.0f;
    const auto heard = run (EffectKind::hushDeEsser, p, mixSig);
    std::printf ("  listen: 300 Hz %+.1f dB, 7 kHz %+.1f dB\n", db (amplitude (heard, 300.0, 22050, 16384) / 0.3),
                 db (amplitude (heard, 7000.0, 22050, 16384) / 0.3));
    CHECK (db (amplitude (heard, 300.0, 22050, 16384) / 0.3) < -30.0);
    CHECK (db (amplitude (heard, 7000.0, 22050, 16384) / 0.3) > -2.0);
}

TEST (mixsuite96_aether_air_and_harmonics)
{
    using namespace mixsuite96;
    using namespace lynx::mixsuite::aether;
    auto p = defaults (EffectKind::aether);
    std::unique_ptr<lynx::Effect> fx;
    // neutral settings: a pure delay of the reported latency
    const auto in = sine (440.0, 0.5, 8192);
    const auto y = run (EffectKind::aether, p, in, 44100.0, 333, &fx);
    const auto lat = fx->latencySamples();
    CHECK (lat == ms::Oversampler::latency());
    double err = 0.0;
    for (std::size_t i = (std::size_t) lat; i < y.size(); ++i)
        err = std::max (err, (double) std::abs (y[i] - in[i - (std::size_t) lat]));
    CHECK (err < 1e-6);

    // air shelf: +10 dB at 10 kHz, measured at 16 kHz and 200 Hz
    p[air] = (float) (10.0 / 15.0);
    p[airFreq] = 0.5f; // 10 kHz
    const auto hi = db (amplitude (run (EffectKind::aether, p, sine (16000.0, 0.1, 22050)), 16000.0, 11025, 8192) / 0.1);
    const auto lo = db (amplitude (run (EffectKind::aether, p, sine (200.0, 0.1, 22050)), 200.0, 11025, 8192) / 0.1);
    std::printf ("  air +10 dB @ 10 kHz: 16 kHz %+.2f dB, 200 Hz %+.2f dB\n", hi, lo);
    CHECK (hi > 8.0 && hi < 10.5);
    CHECK (std::abs (lo) < 0.1);

    // exciter: a 5 kHz tone gets a 2nd harmonic (even character), a 300 Hz tone gets nothing (below the band)
    p = defaults (EffectKind::aether);
    p[harmonics] = 1.0f;
    p[character] = 0.0f;
    p[exciterFreq] = (float) (std::log (3000.0 / 1000.0) / std::log (12.0));
    const auto ex = run (EffectKind::aether, p, sine (5000.0, 0.5, 22050));
    const auto h2 = db (amplitude (ex, 10000.0, 11025, 8192) / amplitude (ex, 5000.0, 11025, 8192));
    const auto low = run (EffectKind::aether, p, sine (300.0, 0.5, 22050));
    const auto l2 = db (amplitude (low, 600.0, 11025, 8192) / amplitude (low, 300.0, 11025, 8192));
    p[character] = 1.0f;
    const auto exOdd = run (EffectKind::aether, p, sine (5000.0, 0.5, 22050));
    const auto o3 = db (amplitude (exOdd, 15000.0, 11025, 8192) / amplitude (exOdd, 5000.0, 11025, 8192));
    const auto o2 = db (amplitude (exOdd, 10000.0, 11025, 8192) / amplitude (exOdd, 5000.0, 11025, 8192));
    std::printf ("  exciter: even H2 %.1f dB; 300 Hz tone H2 %.1f dB; odd H3 %.1f dB, H2 %.1f dB\n", h2, l2, o3, o2);
    CHECK (h2 > -30.0);
    CHECK (l2 < -80.0);
    CHECK (o3 > -40.0 && o2 < -80.0);
}

TEST (mixsuite96_ember_thd_and_aliasing)
{
    using namespace mixsuite96;
    using namespace lynx::mixsuite::ember;
    auto thd = [] (float styleValue, float driveValue, double amp, std::array<double, 3>* harmonics = nullptr) {
        auto p = defaults (EffectKind::ember);
        p[style] = styleValue;
        p[drive] = driveValue;
        const auto y = run (EffectKind::ember, p, sine (1000.0, amp, 32768));
        const auto f = amplitude (y, 1000.0, 16384, 16384);
        double sum = 0.0;
        for (int h = 2; h <= 9; ++h)
        {
            const auto a = amplitude (y, 1000.0 * h, 16384, 16384);
            sum += a * a;
            if (harmonics != nullptr && h <= 4)
                (*harmonics)[(std::size_t) h - 2] = db (a / f);
        }
        return std::sqrt (sum) / f;
    };
    const char* names[] = { "tube", "tape", "transistor", "fuzz" };
    for (int s = 0; s < styles; ++s)
    {
        const auto v = (float) s / (styles - 1);
        CHECK ((int) styleOf (v) == s);
        std::array<double, 3> h {};
        const auto clean = thd (v, 0.0f, 0.01), driven = thd (v, 0.5f, 0.25, &h);
        std::printf ("  %-10s THD clean %.4f%%, +18 dB drive %.2f%% (H2 %.1f, H3 %.1f, H4 %.1f dB)\n", names[s], 100 * clean, 100 * driven, h[0], h[1], h[2]);
        CHECK (clean < 0.01);
        CHECK (driven > 0.01);
        if (s == 0 || s == 3)
            CHECK (h[0] > -40.0); // asymmetric curves: strong 2nd harmonic
        else
            CHECK (h[0] < -80.0 && h[1] > -40.0); // symmetric: odd harmonics only
    }

    // aliasing: a 15 kHz tone driven hard; harmonics fold back to 0.9, 13.2 and 16.8 kHz without oversampling
    auto alias = [] (float quality) {
        auto p = defaults (EffectKind::ember);
        p[style] = 2.0f / 3.0f;
        p[drive] = 2.0f / 3.0f;
        p[lynx::mixsuite::ember::quality] = quality;
        const auto y = run (EffectKind::ember, p, sine (15000.0, 0.5, 32768));
        const auto f = amplitude (y, 15000.0, 16384, 16384);
        double worst = -300.0;
        for (double a : { 900.0, 13200.0, 16800.0, 1200.0 })
            worst = std::max (worst, db (amplitude (y, a, 16384, 16384) / f));
        return worst;
    };
    const auto a2 = alias (0.0f), a8 = alias (1.0f);
    std::printf ("  aliasing at 15 kHz: 2x %.1f dB, 8x %.1f dB\n", a2, a8);
    CHECK (a8 < -70.0);
    CHECK (a8 < a2 - 6.0);

    // mix 0 = the input delayed by the latency, at every quality (the latency never changes)
    for (float q : { 0.0f, 0.5f, 1.0f })
    {
        auto p = defaults (EffectKind::ember);
        p[drive] = 1.0f;
        p[mix] = 0.0f;
        p[quality] = q;
        std::unique_ptr<lynx::Effect> fx;
        const auto in = sine (440.0, 0.5, 4096);
        const auto y = run (EffectKind::ember, p, in, 48000.0, 256, &fx);
        const auto lat = fx->latencySamples();
        double err = 0.0;
        for (std::size_t i = (std::size_t) lat; i < y.size(); ++i)
            err = std::max (err, (double) std::abs (y[i] - in[i - (std::size_t) lat]));
        CHECK (lat == ms::Oversampler::latency() && err < 1e-6);
    }
}

TEST (mixsuite96_registry)
{
    using namespace mixsuite96;
    for (auto kind : { EffectKind::ridgeStrip, EffectKind::glowLeveler, EffectKind::hushDeEsser, EffectKind::aether, EffectKind::ember })
    {
        const auto& info = lynx::effectInfo (kind);
        auto fx = lynx::makeEffect (kind);
        CHECK (fx != nullptr);
        fx->prepare (48000.0);
        const auto before = fx->latencySamples(); // known before the first block (REA round 87)
        auto p = defaults (kind);
        std::vector<float> l (300, 0.1f), r (300, 0.1f);
        fx->process (l.data(), r.data(), 300, p, 120.0);
        CHECK (fx->latencySamples() == before);
        CHECK (! info.params.empty() && info.params.size() <= (std::size_t) lynx::maxEffectParams);
        for (int i = 0; i < (int) info.params.size(); ++i)
            CHECK (! lynx::formatEffectParam (kind, i, info.params[(std::size_t) i].defaultValue, 120.0).empty());
        std::printf ("  %s: %d parameters, latency %d\n", info.name, (int) info.params.size(), before);
    }
}

TEST (mixsuite_panels_place_every_parameter)
{
    using namespace lynx;
    // LYNX's own panels (not FL arrangements): every parameter has exactly one control, controls stay inside the body
    // and never overlap, and the dynamics effects show their live gain-reduction meter
    CHECK (plugui::ownLayoutKinds().size() == 24); // the mixing suite, the reverbs of round 100, the delay / modulation suite of round 101, the dynamics of round 108, the voice tools of round 119, Voice Shift and Auto-pan (round 135), the Hardware Insert (round 164d)
    for (const auto kind : plugui::ownLayoutKinds())
    {
        if (kind == EffectKind::echoform || kind == EffectKind::spiralEcho || kind == EffectKind::orbit || kind == EffectKind::latticeMod
            || kind == EffectKind::autoPan)
            continue; // round 101's panels (and round 135's Auto-pan) swap knobs in place by style / mode / source: checked by their own test
        const auto& l = plugui::layoutFor (kind);
        CHECK (! plugui::hasFlLayout (kind));
        const auto count = (int) effectInfo (kind).params.size();
        std::vector<int> placed ((std::size_t) count, 0);
        std::vector<const plugui::LayoutItem*> controls;
        // round 135: two items shown under disjoint ranges of one mode parameter never show together (Horizon's
        // Drive / Detune and Early-late / Attack swap in place by mode, like the reference)
        // two items never show together when any of their show-conditions on the same parameter are disjoint
        // (round 148: also the second condition, e.g. Ridge's 4K voicing items inside the Vocal strip wrapper)
        auto exclusive = [] (const plugui::LayoutItem& a, const plugui::LayoutItem& b) {
            const char* ap[2] { a.whenParam, a.when2Param };
            const char* bp[2] { b.whenParam, b.when2Param };
            const float alo[2] { a.whenMin, a.when2Min }, ahi[2] { a.whenMax, a.when2Max };
            const float blo[2] { b.whenMin, b.when2Min }, bhi[2] { b.whenMax, b.when2Max };
            for (int i = 0; i < 2; ++i)
                for (int j = 0; j < 2; ++j)
                    if (ap[i] != nullptr && bp[j] != nullptr && std::strcmp (ap[i], bp[j]) == 0 && (ahi[i] <= blo[j] || bhi[j] <= alo[i]))
                        return true;
            return false;
        };
        std::vector<std::vector<const plugui::LayoutItem*>> byParam ((std::size_t) count);
        int meters = 0;
        for (const auto& item : l.items)
        {
            CHECK (item.x >= 0.0f && item.y >= 0.0f && item.x + item.w <= (float) l.width + 0.5f && item.y + item.h <= (float) l.height + 0.5f);
            if (item.param != nullptr && item.type != plugui::Item::display && item.type != plugui::Item::value) // read-outs don't edit
            {
                const auto p = plugui::paramIndex (kind, item.param);
                CHECK (p >= 0);
                if (p >= 0)
                {
                    bool variant = false; // another variant of a control already counted
                    for (const auto* other : byParam[(std::size_t) p])
                        variant = variant || exclusive (*other, item);
                    if (! variant)
                        ++placed[(std::size_t) p];
                    byParam[(std::size_t) p].push_back (&item);
                }
            }
            if (item.type == plugui::Item::tabs || item.type == plugui::Item::radio)
                CHECK (item.values.empty() || item.values.size() == plugui::optionLabels (item).size());
            if (item.type == plugui::Item::display && (std::strcmp (item.display, "grmeter") == 0 || std::strcmp (item.display, "vumeter") == 0
                                                       || std::strcmp (item.display, "gcmeter") == 0)) // round 152: the Compressor's bipolar meter
                ++meters; // round 114: the optical leveler shows its reduction on a VU needle
            if (item.type != plugui::Item::section && item.type != plugui::Item::label)
                controls.push_back (&item);
        }
        for (int p = 0; p < count; ++p)
            if (placed[(std::size_t) p] != 1)
            {
                std::printf ("  %s: parameter '%s' has %d controls\n", effectInfo (kind).name, effectInfo (kind).params[(std::size_t) p].name, placed[(std::size_t) p]);
                CHECK (false);
            }
        for (std::size_t a = 0; a < controls.size(); ++a)
            for (std::size_t b = a + 1; b < controls.size(); ++b)
            {
                const auto& i = *controls[a];
                const auto& j = *controls[b];
                if (i.page >= 0 && j.page >= 0 && i.page != j.page) // different pages never show together
                    continue;
                if (exclusive (i, j))
                    continue;
                const auto overlap = i.x + 0.5f < j.x + j.w && j.x + 0.5f < i.x + i.w && i.y + 0.5f < j.y + j.h && j.y + 0.5f < i.y + i.h;
                if (overlap)
                {
                    std::printf ("  %s: '%s' overlaps '%s'\n", effectInfo (kind).name, i.param != nullptr ? i.param : i.display,
                                 j.param != nullptr ? j.param : j.display);
                    CHECK (false);
                }
            }
        CHECK (meters == (kind == EffectKind::ridgeStrip ? 4 // round 145: + the Vocal strip voicing's two
                          : (kind == EffectKind::glowLeveler || kind == EffectKind::hushDeEsser || kind == EffectKind::crestCompressor
                             || kind == EffectKind::sentryGate || kind == EffectKind::quellDenoiser || kind == EffectKind::compressor
                             || kind == EffectKind::ember) ? 1 : 0)); // round 165: Ember's Classic VU
    }
}

TEST (mixsuite_stands_in_for_missing_vocal_plugins)
{
    using namespace lynx;
    // round 98's substitution table puts the suite in the role of the vocal-chain plugins of the queue (by name)
    const std::pair<const char*, EffectKind> cases[] = { { "FabFilter Pro-DS", EffectKind::hushDeEsser }, { "Fresh Air", EffectKind::aether },
                                                         { "Decapitator", EffectKind::ember }, { "FabFilter Saturn 2", EffectKind::ember },
                                                         { "SSL Native Channel Strip 2", EffectKind::ridgeStrip } };
    for (const auto& [name, kind] : cases)
    {
        PluginIdentity id;
        id.format = ForeignPlugin::Format::vst3;
        id.name = name;
        const auto* entry = findPluginSubstitute (id, false);
        CHECK (entry != nullptr && entry->effect == kind);
    }

    // CLA-2A by its class ID -> Glow Leveler with the table's preset (peak reduction 0.30, gain 0.067; round 113)
    PluginIdentity cla;
    cla.format = ForeignPlugin::Format::vst3;
    cla.name = "WaveShell1-VST3 16.0_x64";
    const auto hex = std::string ("4c545356324163536c612d3261207374");
    for (std::size_t i = 0; i < 16; ++i)
        cla.classId[i] = (std::uint8_t) std::stoi (hex.substr (i * 2, 2), nullptr, 16);
    cla.hasClassId = true;
    const auto* entry = findPluginSubstitute (cla, false);
    CHECK (entry != nullptr && entry->effect == EffectKind::glowLeveler);
    if (entry != nullptr)
    {
        const auto slot = makeSubstituteSlot (*entry);
        CHECK (slot.kind == EffectKind::glowLeveler);
        CHECK (std::abs (slot.params[(std::size_t) mixsuite::glow::peakReduction] - 0.30f) < 1e-6f);
        CHECK (std::abs (slot.params[(std::size_t) mixsuite::glow::gain] - 0.067f) < 1e-6f);
    }
}
