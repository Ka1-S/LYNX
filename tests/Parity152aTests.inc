// Round 152a (docs/analise/REA_rodada152a.md): Compressor ← Renaissance Compressor (the vintage voicing's static curve
// measured at every ratio, the bipolar gain-change meter, an RComp-style factory start) and Glow Leveler ← CLA-2A (the
// reference's defaults, the measured Limit law, AUTO MAKEUP, ANALOG hum and noise, translation of Mix / Auto Makeup /
// Analog). The numbers are the official trials' responses (black-box renders, Waves demo mode, mute-masked).

namespace p152a
{
constexpr double sr = 48000.0;

// up-going 1 kHz stairs (peak dBFS), `seconds` per step; returns the median-free mean gain (dB) of each step's last third
inline std::vector<double> stairGains (const lynx::EffectSlot& slot, const std::vector<double>& levels, double seconds)
{
    const auto per = (std::size_t) (seconds * sr);
    const auto n = per * levels.size();
    std::vector<float> l (n), r (n);
    for (std::size_t i = 0; i < n; ++i)
        l[i] = r[i] = (float) (std::pow (10.0, levels[i / per] / 20.0) * std::sin (2.0 * 3.141592653589793 * 1000.0 * (double) i / sr));
    const auto x = l;
    auto fx = lynx::makeEffect (slot);
    fx->prepare (sr);
    fx->reset();
    for (std::size_t off = 0; off < n; off += 256)
        fx->process (l.data() + off, r.data() + off, (int) std::min<std::size_t> (256, n - off), slot.params, 120.0);
    const auto lat = (std::size_t) std::max (0, fx->latencySamples());
    std::vector<double> out;
    for (std::size_t k = 0; k < levels.size(); ++k)
    {
        double ey = 0.0, ex = 0.0;
        for (auto i = k * per + per * 2 / 3; i < (k + 1) * per - lat; ++i)
        {
            ey += (double) l[i + lat] * l[i + lat];
            ex += (double) x[i] * x[i];
        }
        out.push_back (10.0 * std::log10 (ey / ex));
    }
    return out;
}

inline double rcRatioV (double ratio) { return 0.625 * (1.0 - std::log (ratio) / std::log (50.0)); }
} // namespace p152a

TEST (compressor_measured_vintage_curve_round152a)
{
    using namespace lynx;
    const auto& info = effectInfo (EffectKind::compressor);
    CHECK (info.params.size() == 19 && std::string (info.params[18].name) == "Vintage curve" && info.params[18].defaultValue == 1.0f);
    CHECK (formatEffectParam (EffectKind::compressor, 18, 1.0f, 120.0) == "measured" && formatEffectParam (EffectKind::compressor, 18, 0.0f, 120.0) == "round 150");
    // a project saved before round 152 (18 parameters) keeps the round-150 curve
    auto old = makeEffectSlot (EffectKind::compressor);
    upgradeLoadedEffectParams (old, 18);
    CHECK (old.params[18] == 0.0f);
    auto fresh = makeEffectSlot (EffectKind::compressor);
    upgradeLoadedEffectParams (fresh, 19);
    CHECK (fresh.params[18] == 1.0f);

    // [EXEC] the trial at threshold −30 dB, ARC / Electro / Warm, attack 16 ms, release 160 ms (1 kHz stairs, dB of gain at
    // −24 / −12 / 0 dBFS); 3.3 and 8 : 1 were not used for the table (holdouts)
    struct Case { double ratio, at24, at12, at0; };
    const Case cases[] { { 2.0, -5.55, -11.76, -15.09 }, { 3.3, -6.55, -15.47, -22.34 }, { 8.0, -6.96, -17.44, -28.30 },
                         { 10.46, -6.99, -17.62, -29.03 }, { 50.0, -7.03, -17.82, -29.89 } };
    for (const auto& c : cases)
    {
        auto s = makeEffectSlot (EffectKind::compressor);
        s.params[0] = 0.5f;   // −30 dB
        s.params[8] = 1.0f;   // the vintage voicing
        s.params[13] = 1.0f;  // the RComp ranges
        s.params[14] = (float) p152a::rcRatioV (c.ratio);
        s.params[16] = s.params[17] = 0.5017f; // 16 ms, 160 ms
        const auto g = p152a::stairGains (s, { -60, -24, -12, 0 }, 0.8);
        auto o = s;
        o.params[18] = 0.0f;
        const auto go = p152a::stairGains (o, { -60, -24, -12, 0 }, 0.8);
        std::printf ("  Compressor vintage %.2f:1: %+.2f / %+.2f / %+.2f dB (RComp %+.2f / %+.2f / %+.2f; round 150 %+.2f / %+.2f / %+.2f)\n",
                     c.ratio, g[1], g[2], g[3], c.at24, c.at12, c.at0, go[1], go[2], go[3]);
        CHECK (std::abs (g[1] - c.at24) < 0.25 && std::abs (g[2] - c.at12) < 0.25 && std::abs (g[3] - c.at0) < 0.25);
    }
    // Smooth has Warm's static curve on the trial (identical stairs)
    {
        auto s = makeEffectSlot (EffectKind::compressor);
        s.params[0] = 0.5f; s.params[8] = 1.0f; s.params[13] = 1.0f; s.params[14] = (float) p152a::rcRatioV (4.0);
        auto m = s;
        m.params[11] = 1.0f; // Smooth
        const auto a = p152a::stairGains (s, { -24, -6 }, 0.8), b = p152a::stairGains (m, { -24, -6 }, 0.8);
        CHECK (std::abs (a[0] - b[0]) < 0.05 && std::abs (a[1] - b[1]) < 0.05);
    }
    // her state (−15 dB, 2.95 : 1): the measured curve leaves it as it was up to −3 dBFS
    {
        auto s = makeEffectSlot (EffectKind::compressor);
        s.params[0] = 0.75f; s.params[8] = 1.0f; s.params[13] = 1.0f; s.params[14] = (float) p152a::rcRatioV (2.95);
        s.params[16] = 0.0f; // 0.5 ms
        auto o = s;
        o.params[18] = 0.0f;
        const std::vector<double> lv { -30, -21, -15, -9, -3 };
        const auto a = p152a::stairGains (s, lv, 0.6), b = p152a::stairGains (o, lv, 0.6);
        for (std::size_t k = 0; k < lv.size(); ++k)
            CHECK (std::abs (a[k] - b[k]) < 0.01);
    }
}

TEST (compressor_bipolar_gain_change_meter_round152a)
{
    using namespace lynx;
    // control output 5 = the signed gain change (dB): negative while compressing, positive for the expansion below 1 : 1
    const auto change = [] (int voicing, double ratio) {
        auto s = makeEffectSlot (EffectKind::compressor);
        s.params[0] = 0.5f; s.params[8] = (float) voicing; s.params[13] = 1.0f; s.params[14] = (float) (ratio >= 1.0 ? p152a::rcRatioV (ratio) : 0.625 + 0.375 * std::log (ratio) / std::log (0.5));
        auto fx = makeEffect (s);
        fx->prepare (48000.0);
        std::vector<float> l (512), r (512);
        float last = 0.0f;
        for (int b = 0; b < 100; ++b)
        {
            for (int i = 0; i < 512; ++i)
                l[(std::size_t) i] = r[(std::size_t) i] = (float) (0.25 * std::sin (2.0 * 3.141592653589793 * 1000.0 * (double) (b * 512 + i) / 48000.0));
            fx->process (l.data(), r.data(), 512, s.params, 120.0);
            last = fx->controlOutput (5);
        }
        return last;
    };
    const auto vc = change (1, 4.0), ve = change (1, 0.5), cc = change (0, 4.0), ce = change (0, 0.5);
    std::printf ("  gain change: vintage %+.1f / %+.1f dB, classic %+.1f / %+.1f dB (4 : 1 / 0.5 : 1 at -12 dBFS)\n", vc, ve, cc, ce);
    CHECK (vc < -8.0 && ve > 5.0 && cc < -8.0 && ce > 5.0);
    // the RComp-style factory start: the reference's fresh instance (threshold 0 dB, 1.00 : 1, 16 / 160 ms, limiter)
    for (const auto& p : presets::factory (EffectKind::compressor))
        if (p.name == "RComp-style start")
        {
            std::vector<std::string> unresolved;
            const auto q = presets::apply (EffectKind::compressor, p, 120.0, &unresolved);
            CHECK (unresolved.empty());
            CHECK (formatEffectParam (EffectKind::compressor, 0, q[0], 120.0) == "0.0 dB" && q[13] >= 0.5f && q[8] >= 0.5f && q[12] >= 0.5f);
            CHECK (formatEffectParam (EffectKind::compressor, 14, q[14], 120.0) == "1.00:1");
        }
    CHECK (presets::factory (EffectKind::compressor).size() >= 4);
}

TEST (glow_cla2a_defaults_limit_automakeup_analog_round152a)
{
    using namespace lynx;
    namespace g = mixsuite::glow;
    const auto& info = effectInfo (EffectKind::glowLeveler);
    CHECK (info.params.size() == (std::size_t) g::count && g::analog == 9); // round 165w: + Opto release
    // a new instance starts as the reference does: Opto classic, Gain 40, Peak reduction 46, Hi Freq 50, the measured law
    const auto fresh = makeEffectSlot (EffectKind::glowLeveler);
    CHECK (fresh.params[g::voicing] == 1.0f && std::abs (fresh.params[g::gain] - 0.40f) < 1e-6f && std::abs (fresh.params[g::peakReduction] - 0.46f) < 1e-6f);
    CHECK (std::abs (fresh.params[g::emphasis] - 0.5f) < 1e-6f && fresh.params[g::optoLaw] == 1.0f && fresh.params[g::analog] == 0.0f);
    CHECK (formatEffectParam (EffectKind::glowLeveler, g::analog, 0.5f, 120.0) == "50 Hz" && formatEffectParam (EffectKind::glowLeveler, g::analog, 1.0f, 120.0) == "60 Hz");
    auto old = fresh;
    upgradeLoadedEffectParams (old, 8);
    CHECK (old.params[g::optoLaw] == 0.0f);

    // [EXEC] Limit at peak reduction 62.6 / 80 (Gain 40, Hi Freq 50), up-going 1 kHz stairs 0.5 s per step: the trial
    // over-compresses at 62.6 (−13.7 dB of gain at −6 dBFS, −19.6 at 0) and stops at a 30.4 dB reduction at 80 (−22.4 at 0)
    const std::vector<double> lv { -48, -45, -42, -39, -36, -33, -30, -27, -24, -21, -18, -15, -12, -9, -6, -3, 0 };
    struct Case { double pr, at6, at0; };
    for (const auto& c : { Case { 62.6, -13.7, -19.6 }, Case { 80.0, -21.5, -22.4 } })
    {
        auto s = fresh;
        s.params[g::mode] = 1.0f;
        s.params[g::peakReduction] = (float) (c.pr / 100.0);
        const auto v = p152a::stairGains (s, lv, 0.5);
        auto o = s;
        o.params[g::optoLaw] = 0.0f;
        const auto w = p152a::stairGains (o, lv, 0.5);
        std::printf ("  Glow Limit, PR %.1f: %+.1f / %+.1f dB at -6 / 0 dBFS (CLA-2A %+.1f / %+.1f; round 123 %+.1f / %+.1f)\n",
                     c.pr, v[14], v[16], c.at6, c.at0, w[14], w[16]);
        CHECK (std::abs (v[14] - c.at6) < 1.2 && std::abs (v[16] - c.at0) < 1.2);
    }

    // [EXEC] AUTO MAKEUP: +6.78 dB at peak reduction 62.6, +11.37 at 80, none at 46 (a −40 dBFS tone); the round-123 law
    // (older projects) leaves it out in the classic voicing
    for (const auto& [pr, want] : { std::pair { 46.0, 0.0 }, std::pair { 62.6, 6.78 }, std::pair { 80.0, 11.37 } })
    {
        auto s = fresh;
        s.params[g::peakReduction] = (float) (pr / 100.0);
        auto a = s;
        a.params[g::autoMakeup] = 1.0f;
        const auto off = vm123::sineGainDb (s, 1000.0, -40.0), on = vm123::sineGainDb (a, 1000.0, -40.0);
        CHECK (std::abs (on - off - want) < 0.05);
        a.params[g::optoLaw] = 0.0f;
        CHECK (std::abs (vm123::sineGainDb (a, 1000.0, -40.0) - off) < 0.01);
    }

    // [EXEC] ANALOG on silence: −89 dBFS RMS of noise and hum (100 / 120 Hz the strongest), in the wet path (Trim moves it,
    // Mix 0 % removes it); off = digital silence
    const auto silence = [] (EffectSlot s, double* hum100 = nullptr) {
        const std::size_t n = 48000 * 2;
        std::vector<float> l (n, 0.0f), r (n, 0.0f);
        auto fx = makeEffect (s);
        fx->prepare (48000.0);
        for (std::size_t off = 0; off < n; off += 256)
            fx->process (l.data() + off, r.data() + off, 256, s.params, 120.0);
        double e = 0.0, re = 0.0, im = 0.0;
        for (std::size_t i = 24000; i < n; ++i)
        {
            e += (double) l[i] * l[i];
            const auto ph = 2.0 * 3.141592653589793 * 100.0 * (double) i / 48000.0;
            re += l[i] * std::cos (ph);
            im += l[i] * std::sin (ph);
        }
        if (hum100 != nullptr)
            *hum100 = 20.0 * std::log10 (2.0 * std::hypot (re, im) / (double) (n - 24000) + 1e-30);
        return 10.0 * std::log10 (e / (double) (n - 24000) + 1e-30);
    };
    auto s = fresh;
    s.params[g::analog] = 0.5f;
    double hum = 0.0;
    const auto rms = silence (s, &hum);
    std::printf ("  Glow ANALOG 50 Hz: %.1f dBFS RMS, 100 Hz %.1f dBFS peak (CLA-2A -89.0 / -98.7)\n", rms, hum);
    CHECK (std::abs (rms - -89.0) < 1.0 && std::abs (hum - -98.7) < 1.5);
    auto t = s;
    t.params[g::trim] = 1.0f; // +18 dB
    CHECK (std::abs (silence (t) - rms - 18.0) < 0.3);
    auto m = s;
    m.params[g::mix] = 0.0f;
    CHECK (silence (m) < -200.0 && silence (fresh) < -200.0);
}

TEST (glow_cla2a_translation_round152a)
{
    using namespace lynx;
    namespace g = mixsuite::glow;
    // the trial's RealWorld list (208 values) at its defaults, then Mix 37 %, Auto Makeup on, Analog 60 Hz — read back
    // through its own parameters (REA_rodada152a §2): 204 Mix, 207 Auto Makeup, 140 Analog (0 = 50 Hz, 1 = 60, 2 = off)
    std::vector<std::string> v (208, "0");
    v[38] = "40"; v[74] = "46"; v[112] = "0"; v[153] = "50"; v[206] = "0";
    v[204] = "37"; v[207] = "1"; v[140] = "1";
    std::string list;
    for (const auto& x : v)
        list += x + " ";
    int n = 0;
    const auto s = vm123::wavesSlot ("CLA-2A", list.c_str(), n);
    CHECK (s.kind == EffectKind::glowLeveler && n == 8);
    CHECK (std::abs (s.params[g::mix] - 0.37f) < 1e-4f && s.params[g::autoMakeup] == 1.0f && s.params[g::analog] == 1.0f && s.params[g::optoLaw] == 1.0f);
    v[140] = "2"; v[204] = "100"; v[207] = "0";
    list.clear();
    for (const auto& x : v)
        list += x + " ";
    const auto o = vm123::wavesSlot ("CLA-2A", list.c_str(), n);
    CHECK (o.params[g::analog] == 0.0f && o.params[g::mix] == 1.0f && o.params[g::autoMakeup] == 0.0f);
}
