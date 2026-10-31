// --- Round 152c: Facet EQ ← Pro-Q 4 — spectral dynamics (measured black-box on the trial) and EQ match ---------------

namespace p152c
{
using namespace lynx::facet;
// a dynamic bell at 1 kHz, Q 1, range −12 dB, threshold `thr`, spectral on
inline lynx::EffectParams spectralBell (double thr, bool spectral = true)
{
    auto p = facet103::bell (1000.0, 0.0, 1.0);
    p[(std::size_t) rangeParam0] = rangeValue (-12.0);
    p[(std::size_t) thresholdParam0] = thresholdValue (thr);
    p[(std::size_t) spectralParam0] = spectral ? 1.0f : 0.0f;
    return p;
}
// the steady level change (dB) of each tone of a mix of sines through the effect, read with a long FFT after the latency
inline std::vector<double> toneGains (const lynx::EffectParams& p, const std::vector<double>& hz, const std::vector<double>& db)
{
    const double sr = 48000.0;
    const int n = (int) (sr * 3.0);
    std::vector<float> l ((std::size_t) n), r;
    for (int i = 0; i < n; ++i)
    {
        double x = 0.0;
        for (std::size_t k = 0; k < hz.size(); ++k) x += std::pow (10.0, db[k] / 20.0) * std::sin (2.0 * 3.14159265358979 * hz[k] * i / sr + 0.7 * (double) k);
        l[(std::size_t) i] = (float) x;
    }
    r = l;
    auto in = l;
    auto fx = make (&p);
    fx->prepare (sr);
    for (int o = 0; o < n; o += 512) fx->process (l.data() + o, r.data() + o, std::min (512, n - o), p, 120.0);
    const int lat = fx->latencySamples();
    // a DFT of each tone over 1 s (a whole number of its cycles is not needed: 1 s of a Hann window)
    std::vector<double> out;
    for (std::size_t k = 0; k < hz.size(); ++k)
    {
        std::complex<double> a {}, b {};
        const int len = 48000, s0 = n - len - 100;
        for (int i = 0; i < len; ++i)
        {
            const double w = 0.5 - 0.5 * std::cos (2.0 * 3.14159265358979 * i / len);
            const auto e = std::polar (w, -2.0 * 3.14159265358979 * hz[k] * (s0 + i) / sr);
            a += (double) l[(std::size_t) (s0 + i)] * e;
            b += (double) in[(std::size_t) (s0 + i - lat)] * e;
        }
        out.push_back (20.0 * std::log10 (std::abs (a) / std::abs (b)));
    }
    return out;
}
} // namespace p152c

TEST (facet_spectral_params_round152c)
{
    using namespace lynx::facet;
    CHECK ((int) info().params.size() == paramCount && paramCount >= 114 && paramCount <= lynx::maxEffectParams);
    auto p = facet103::defaults();
    for (int b = 0; b < bands; ++b)
        CHECK (p[(std::size_t) (spectralParam0 + b)] == 0.0f && p[(std::size_t) (densityParam0 + b)] == 0.5f && ! isSpectral (p, b));
    CHECK (paramText (spectralParam0, 1.0f) == "Spectral" && paramText (densityParam0 + 2, 0.25f) == "25%" && paramText (spectralTiltParam0, 1.0f) == "On");
    // a spectral band needs a dynamic range; its latency is the trial's 5120 samples at 48 kHz
    auto q = p152c::spectralBell (-40.0);
    CHECK (isSpectral (q, 0) && spectralLatency (48000.0) == 5120 && spectralLatency (96000.0) == 10240);
    auto fx = make (&q);
    fx->prepare (48000.0);
    CHECK (fx->latencySamples() == 5120);
    q[(std::size_t) rangeParam0] = 0.5f; // a static band is not spectral
    CHECK (! isSpectral (q, 0));
}

TEST (facet_spectral_law_round152c)
{
    using namespace lynx::facet;
    // [EXEC] Pro-Q 4 (D:\lynx-work\parity152\c): partials at −60 dBFS every ¼ octave from 250 Hz, a loud tone at
    // 1 kHz; threshold −40, range −12, Q 1. The loud tone is cut 12 dB, the quieter partials are left alone.
    const std::vector<double> f { 707.1, 841.0, 1000.0, 1189.0, 1414.0 };
    for (const double loud : { -40.0, -30.0, -10.0 })
    {
        std::vector<double> lv { -60.0, -60.0, loud, -60.0, -60.0 }, g (f.size());
        spectralGainsDb (p152c::spectralBell (-40.0), 0, 48000.0, f.data(), lv.data(), (int) f.size(), g.data());
        CHECK (std::abs (g[2] + 12.0) < 0.3);
        for (const int k : { 0, 1, 3, 4 }) CHECK (std::abs (g[(std::size_t) k]) < 0.5);
    }
    // a single tone: −0.5 / −5.8 / −11.1 dB at −70 / −65 / −60 dBFS (threshold −40)
    const double one[3][2] { { -70.0, -0.5 }, { -65.0, -5.8 }, { -60.0, -11.1 } };
    for (const auto& c : one)
    {
        double hz = 1000.0, lv = c[0], g = 0.0;
        spectralGainsDb (p152c::spectralBell (-40.0), 0, 48000.0, &hz, &lv, 1, &g);
        std::printf ("  Facet spectral tone %.0f dBFS: %.2f dB (Pro-Q 4 %.1f)\n", c[0], g, c[1]);
        CHECK (std::abs (g - c[1]) < 0.6);
    }
    // the loud tone off the centre is cut by the range × the shape² … : 700 Hz −30 dBFS → −7.5 (Pro-Q 4)
    {
        double hz = 700.0, lv = -30.0, g = 0.0;
        spectralGainsDb (p152c::spectralBell (-40.0), 0, 48000.0, &hz, &lv, 1, &g);
        std::printf ("  Facet spectral 700 Hz: %.2f dB (Pro-Q 4 -7.5)\n", g);
        CHECK (std::abs (g + 7.5) < 0.9);
    }
}

TEST (facet_spectral_render_round152c)
{
    using namespace lynx::facet;
    // rendered: a loud 1 kHz tone with a quiet 1.41 kHz partial — the spectral band cuts only the loud one; the plain
    // dynamic band (the same settings, Spectral off) pulls the partial down with it (Pro-Q 4: −12.0 / 0.0 vs −11.9 / −9.2)
    const std::vector<double> f { 1000.0, 1414.0 }, db { -10.0, -60.0 };
    const auto sp = p152c::toneGains (p152c::spectralBell (-40.0), f, db);
    const auto dy = p152c::toneGains (p152c::spectralBell (-40.0, false), f, db);
    std::printf ("  Facet spectral render: %.2f / %.2f dB; dynamic %.2f / %.2f dB\n", sp[0], sp[1], dy[0], dy[1]);
    CHECK (std::abs (sp[0] + 12.0) < 1.0 && std::abs (sp[1]) < 0.7);
    CHECK (dy[1] < -6.0);
    // density 100 %: a pure tone is cut a little less (Pro-Q 4: −11.2)
    auto d1 = p152c::spectralBell (-40.0);
    d1[(std::size_t) densityParam0] = 1.0f;
    const auto sd = p152c::toneGains (d1, f, db);
    std::printf ("  Facet spectral density 100 %%: %.2f dB\n", sd[0]);
    CHECK (sd[0] > sp[0] + 0.2 && sd[0] < -9.5);
}

TEST (facet_eq_match_round152c)
{
    using namespace lynx::facet;
    // the reference = the current spectrum + a +6 dB bell at 2 kHz and a −4 dB low shelf at 120 Hz: the match puts
    // bands that rebuild that difference within 1 dB (rms, 40 Hz … 16 kHz)
    auto want = facet103::defaults();
    want[(std::size_t) param (0, 0)] = shapeValue (Shape::bell);
    want[(std::size_t) param (0, 1)] = hzValue (2000.0);
    want[(std::size_t) param (0, 2)] = gainValue (6.0);
    want[(std::size_t) param (1, 0)] = shapeValue (Shape::lowShelf);
    want[(std::size_t) param (1, 1)] = hzValue (120.0);
    want[(std::size_t) param (1, 2)] = gainValue (-4.0);
    const int n = 160;
    std::vector<double> f ((std::size_t) n), cur ((std::size_t) n), ref ((std::size_t) n);
    for (int i = 0; i < n; ++i)
    {
        f[(std::size_t) i] = 20.0 * std::pow (1000.0, (double) i / (n - 1));
        cur[(std::size_t) i] = -40.0 - 3.0 * std::log2 (f[(std::size_t) i] / 1000.0); // a pink-ish programme
        ref[(std::size_t) i] = cur[(std::size_t) i] + responseDb (want, 48000.0, f[(std::size_t) i]) + 3.0; // + a level offset
    }
    auto empty = facet103::defaults();
    std::vector<std::pair<int, float>> changes;
    const int placed = matchEq (empty, f.data(), cur.data(), ref.data(), n, 1.0, 8, 1.0 / 6.0, changes);
    for (const auto& [p, v] : changes) empty[(std::size_t) p] = v;
    double err = 0.0, mean = 0.0;
    int cnt = 0;
    for (int i = 0; i < n; ++i)
        if (f[(std::size_t) i] >= 40.0 && f[(std::size_t) i] <= 16000.0)
        {
            const double e = responseDb (empty, 48000.0, f[(std::size_t) i]) - responseDb (want, 48000.0, f[(std::size_t) i]);
            mean += e; err += e * e; ++cnt;
        }
    mean /= cnt;
    err = std::sqrt (std::max (0.0, err / cnt - mean * mean));
    std::printf ("  Facet EQ match: %d bands, %.2f dB rms off the wanted curve (level %.2f dB)\n", placed, err, mean);
    CHECK (placed >= 2 && placed <= 8 && err < 1.0);
    // amount 0 or identical spectra: nothing to do
    CHECK (matchEq (facet103::defaults(), f.data(), cur.data(), cur.data(), n, 1.0, 8, 1.0 / 6.0, changes) == 0 && changes.empty());
}
