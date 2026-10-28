// round 165h (REA_rodada165h): Ridge Strip ← SSL E-Channel — the console's ∞ : 1 (state value 100) and its law, the
// EQ to the dynamics' side chain (Dyn S.C.), measured black-box on the official trial

TEST (ridge_e_channel_infinite_ratio_round165)
{
    using namespace lynx;
    namespace r = mixsuite::ridge;
    // her state with the ratio at ∞ (the trial stores it as 100), threshold 0, fast attack, EQ / filters off
    int n = 0;
    auto s = vm123::wavesSlot ("SSLChannel", "0 100 1 0.4 * -30 0 0 0 0.2 * 0 0 0 0 60 0 0.5 2.5 0 3.5 1.5 0 0 8 1 0 0 30 1 0 0 * * 1 * * 7 -18 0 0 * * * * * * -18 -18 -18 -18 -18 -18 -18 -18 0 0", n);
    CHECK (s.kind == EffectKind::ridgeStrip && s.params[r::compRatio] == 1.0f);
    // [EXEC] the trial's stairs (1 kHz, −60 … 0 dBFS in 2 dB steps of 0.5 s, peak level over each step's second half):
    // the gain at −60 / −30 / −12 / 0 dBFS
    static constexpr double want[][2] = { { -60, 1.98 }, { -30, 2.38 }, { -12, -9.66 }, { 0, -21.87 } };
    const auto step = (std::size_t) (0.5 * vm123::sr);
    std::vector<float> l (step * 31), rr;
    for (std::size_t i = 0; i < l.size(); ++i)
        l[i] = (float) (std::pow (10.0, (-60.0 + 2.0 * (double) (i / step)) / 20.0) * std::sin (2.0 * 3.141592653589793 * 1000.0 * (double) i / vm123::sr));
    const auto x = l;
    rr = l;
    auto fx = makeEffect (s);
    fx->prepare (vm123::sr);
    fx->reset();
    for (std::size_t off = 0; off < l.size(); off += 256)
        fx->process (l.data() + off, rr.data() + off, (int) std::min<std::size_t> (256, l.size() - off), s.params, 120.0);
    for (const auto& w : want)
    {
        const auto k = (std::size_t) ((w[0] + 60.0) / 2.0);
        double ey = 0.0, ex = 0.0;
        for (auto i = k * step + step / 2; i < (k + 1) * step; ++i)
        {
            ey += (double) l[i] * l[i];
            ex += (double) x[i] * x[i];
        }
        const auto got = 10.0 * std::log10 (ey / ex);
        std::printf ("  E-Channel inf:1 at %.0f dBFS: %.2f dB (trial %.2f)\n", w[0], got, w[1]);
        CHECK (std::abs (got - w[1]) < (w[0] > -1.0 ? 2.0 : 0.6)); // 1.4 dB at 0 dBFS (round 123's fit: about 8 dB)
    }
}

TEST (ridge_e_channel_eq_to_sidechain_round165)
{
    using namespace lynx;
    namespace r = mixsuite::ridge;
    // HF shelf +15 dB at 8 kHz, compressor 1 : 1 (off): with Dyn S.C. on the EQ leaves the audio, so the 8 kHz tone
    // passes flat; with it off it gets the boost
    const char* off = "0 1 1 0.35 * -30 0 0 0 0.2 * 0 0 0 0 60 0 0.5 2.5 0 3.5 1.5 0 15 8 0 0 0 30 1 0 0 * * 1 * * 7 -18 0 0 * * * * * * -18 -18 -18 -18 -18 -18 -18 -18 0 0";
    const char* on = "0 1 1 0.35 * -30 0 0 0 0.2 * 0 0 0 0 60 0 0.5 2.5 0 3.5 1.5 0 15 8 0 1 0 30 1 0 0 * * 1 * * 7 -18 0 0 * * * * * * -18 -18 -18 -18 -18 -18 -18 -18 0 0";
    int n = 0;
    auto a = vm123::wavesSlot ("SSLChannel", off, n);
    auto b = vm123::wavesSlot ("SSLChannel", on, n);
    CHECK (a.params[r::eqToSidechain] == 0.0f && b.params[r::eqToSidechain] == 1.0f);
    const auto ga = vm123::sineGainDb (a, 8000.0, -30.0), gb = vm123::sineGainDb (b, 8000.0, -30.0);
    std::printf ("  E-Channel EQ to SC: 8 kHz gain %.2f dB (EQ in the audio) / %.2f dB (EQ to the side chain)\n", ga, gb);
    CHECK (ga > 10.0 && std::abs (gb) < 0.3);

    // de-essing: ∞ : 1 at −10 with the HF shelf in the side chain turns an 8 kHz tone down more than a 200 Hz one
    // of the same level
    const char* dess = "-10 100 1 0.35 * -30 0 0 0 0.2 * 0 0 0 0 60 0 0.5 2.5 0 3.5 1.5 0 15 8 0 1 0 30 1 0 0 * * 1 * * 7 -18 0 0 * * * * * * -18 -18 -18 -18 -18 -18 -18 -18 0 0";
    auto c = vm123::wavesSlot ("SSLChannel", dess, n);
    const auto hi = vm123::sineGainDb (c, 8000.0, -12.0), lo = vm123::sineGainDb (c, 200.0, -12.0);
    std::printf ("  E-Channel de-essing: 8 kHz %.2f dB, 200 Hz %.2f dB\n", hi, lo);
    CHECK (hi < lo - 6.0);

    // Ridge's own engine: the same switch keeps the EQ out of the audio
    auto own = makeEffectSlot (EffectKind::ridgeStrip);
    own.params[r::hfGain] = 1.0f;
    own.params[r::eqToSidechain] = 1.0f;
    CHECK (std::abs (vm123::sineGainDb (own, 8000.0, -30.0)) < 0.3);
}
