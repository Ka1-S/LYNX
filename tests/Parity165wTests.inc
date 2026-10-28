// round 165w (REA_rodada165w): Gemini Doubler ← Doubler2 / Doubler4 — the shelves' measured law and range (−88 … +12 dB);
// Glow Leveler ← CLA-2A — the classic voicing's release, measured black-box on the official 17.1 trials

TEST (gemini_doubler_measured_shelves_round165w)
{
    using namespace lynx;
    namespace g = voicetools::gemini;
    auto base = makeEffectSlot (EffectKind::geminiDoubler).params;
    CHECK (base[g::eqLaw] == 1.0f && g::paramCount == (int) effectInfo (EffectKind::geminiDoubler).params.size());
    CHECK (std::abs (g::shelfDb (0.0f) + 88.0) < 1e-9 && std::abs (g::shelfDb (1.0f) - 12.0) < 1e-9 && std::abs (g::shelfDb (0.0f, false) + 30.0) < 1e-9);
    CHECK (formatEffectParam (EffectKind::geminiDoubler, g::lowGain, 0.0f, 120.0, base) == "-88.0 dB");
    // one voice alone (0 dB, centre, no delay or detune, own side), the direct signal off
    base[g::directOn] = 0.0f;
    base[(std::size_t) g::voiceParam (1, g::vOn)] = 0.0f;
    base[(std::size_t) g::voiceParam (0, g::vGain)] = 1.0f;
    base[(std::size_t) g::voiceParam (0, g::vPan)] = 0.5f;
    base[(std::size_t) g::voiceParam (0, g::vDelay)] = 0.0f;
    base[(std::size_t) g::voiceParam (0, g::vDetune)] = 0.5f;
    base[(std::size_t) g::voiceParam (0, g::vDepth)] = 0.5f;
    base[g::lowOn] = base[g::highOn] = 0.0f;
    const auto level = [&] (const EffectParams& p, double hz) {
        const auto y = vt119::run (EffectKind::geminiDoubler, p, vt119::tone (hz, -20.0, 0.5));
        return vt119::rmsDb (y.l, y.l.size() / 2, y.l.size());
    };
    // [EXEC] the trial's response (stepped tones, voice only): high shelf −88 dB at 2448 Hz, low shelf −88 dB at 281 Hz
    // and −24 dB at 1189 Hz, +12 dB high shelf at 2448 Hz
    struct Point { bool high; double db, f0, hz, want; };
    static constexpr Point points[] = { { true, -88, 2448, 5024, -8.9 }, { true, -88, 2448, 12619, -25.6 }, { true, -88, 2448, 1262, -0.9 },
                                        { false, -88, 281, 50, -22.8 }, { false, -88, 281, 126, -9.8 }, { false, -24, 1189, 200, -18.2 },
                                        { true, 12, 2448, 3170, 6.9 }, { true, -12, 2448, 7962, -8.5 } };
    for (const auto& pt : points)
    {
        auto off = base, p = base;
        p[pt.high ? g::highOn : g::lowOn] = 1.0f;
        p[pt.high ? g::highGain : g::lowGain] = g::shelfValue (pt.db);
        p[pt.high ? g::highFreq : g::lowFreq] = g::freqValue (pt.f0);
        const auto got = level (p, pt.hz) - level (off, pt.hz);
        if (std::abs (got - pt.want) >= 0.3)
            std::printf ("  shelf %s %.0f dB @ %.0f Hz, %.0f Hz: %.2f (trial %.2f)\n", pt.high ? "high" : "low", pt.db, pt.f0, pt.hz, got, pt.want);
        CHECK (std::abs (got - pt.want) < 0.3);
        // the editor's curve (the engine's response at 48 kHz) agrees
        CHECK (std::abs (g::shelfResponseDb (pt.hz, pt.f0, pt.db, pt.high, true) - got) < 0.1);
    }
    // round 124's law stays for projects saved before (one first-order section, −30 … +12)
    {
        auto p = base;
        p[g::eqLaw] = 0.0f;
        p[g::highOn] = 1.0f;
        p[g::highGain] = g::shelfValue (-12.0, false);
        p[g::highFreq] = g::freqValue (2448.0);
        const auto got = level (p, 7962.0) - level (base, 7962.0);
        CHECK (std::abs (got - g::shelfResponseDb (7962.0, 2448.0, -12.0, true, false)) < 0.1);
        CHECK (formatEffectParam (EffectKind::geminiDoubler, g::highGain, g::shelfValue (-12.0, false), 120.0, p) == "-12.0 dB");
    }
}

TEST (gemini_doubler_older_projects_keep_their_shelves_round165w)
{
    using namespace lynx;
    namespace g = voicetools::gemini;
    // a project saved before round 165w has no EQ law: it loads with round 124's shelves (its gains mean what they meant)
    auto slot = makeEffectSlot (EffectKind::geminiDoubler);
    upgradeLoadedEffectParams (slot, (std::size_t) g::eqLaw);
    CHECK (slot.params[g::eqLaw] == 0.0f);
    slot = makeEffectSlot (EffectKind::geminiDoubler);
    upgradeLoadedEffectParams (slot, (std::size_t) g::paramCount);
    CHECK (slot.params[g::eqLaw] == 1.0f);
    // a user preset written before has no "EQ law" line: the same
    auto params = makeEffectSlot (EffectKind::geminiDoubler).params;
    params[g::lowGain] = 30.0f / 42.0f; // 0 dB on round 124's law
    auto text = presets::toText (EffectKind::geminiDoubler, "Old double", params);
    text = text.substr (0, text.find ("EQ law"));
    const auto back = presets::fromText (EffectKind::geminiDoubler, text);
    CHECK (back && back->params[g::eqLaw] == 0.0f && std::abs (g::shelfDb (back->params[g::lowGain], false)) < 1e-4);
    const auto full = presets::fromText (EffectKind::geminiDoubler, presets::toText (EffectKind::geminiDoubler, "New", makeEffectSlot (EffectKind::geminiDoubler).params));
    CHECK (full && full->params[g::eqLaw] == 1.0f);
}

TEST (gemini_doubler_translates_deep_shelf_cuts_round165w)
{
    using namespace lynx;
    namespace g = voicetools::gemini;
    PluginIdentity id;
    id.format = ForeignPlugin::Format::vst3;
    id.name = "WaveShell1-VST3 17.1_x64";
    // shelves on: low −60 dB at 841 Hz, high −88 dB at 3538 Hz (the reference's deep cuts)
    id.state = substtest::wavesComponent ("Doubler", "0 1 0 0 1 * * * * 1 -60 841 3538 -88 1 16 0 0 0 0 64 20 32 -60 -40 -20 1 0 -45 0 -1 78.2 39 0 1 * "
                                                     "1 0 45 0 0 100 39 0 1 * 0 0 -45 0 0 100 0 0 1 * 0 0 45 0 0 100 0 0 1 * * * *");
    const auto* entry = findPluginSubstitute (id, false);
    CHECK (entry != nullptr);
    if (entry == nullptr)
        return;
    int n = 0;
    const auto s = makeSubstituteSlot (*entry, &id, &n);
    CHECK (s.kind == EffectKind::geminiDoubler && s.params[g::eqLaw] == 1.0f);
    CHECK (std::abs (g::shelfDb (s.params[g::lowGain]) + 60.0) < 1e-4 && std::abs (g::shelfDb (s.params[g::highGain]) + 88.0) < 1e-4);
    CHECK (s.params[g::lowOn] == 1.0f && s.params[g::highOn] == 1.0f);
}

// Glow's classic voicing: a 1 kHz burst, then a −40 dBFS probe; the gain (dB, against the probe's own level before the
// burst) at times after the burst ends
namespace p165w
{
inline std::vector<double> glowRecovery (const lynx::EffectParams& p, double burstDb, double burstSecs, const std::vector<double>& at)
{
    using namespace lynx;
    constexpr double sr = 48000.0;
    const auto pre = (std::size_t) sr, burst = (std::size_t) (burstSecs * sr), post = (std::size_t) (16.0 * sr);
    std::vector<float> l (pre + burst + post), r;
    for (std::size_t i = 0; i < l.size(); ++i)
        l[i] = (float) (std::pow (10.0, ((i >= pre && i < pre + burst) ? burstDb : -40.0) / 20.0) * std::sin (2.0 * 3.141592653589793 * 1000.0 * (double) i / sr));
    const auto x = l;
    r = l;
    auto slot = makeEffectSlot (EffectKind::glowLeveler);
    slot.params = p;
    auto fx = makeEffect (slot);
    fx->prepare (sr);
    fx->reset();
    for (std::size_t off = 0; off < l.size(); off += 256)
        fx->process (l.data() + off, r.data() + off, (int) std::min<std::size_t> (256, l.size() - off), p, 120.0);
    const auto gainAt = [&] (std::size_t from) { // 10 ms window
        double ey = 0.0, ex = 0.0;
        for (auto i = from; i < from + 480; ++i) { ey += (double) l[i] * l[i]; ex += (double) x[i] * x[i]; }
        return 10.0 * std::log10 (ey / ex);
    };
    const auto base = gainAt (pre / 2);
    std::vector<double> out;
    for (const auto t : at)
        out.push_back (gainAt (pre + burst + (std::size_t) (t * sr)) - base);
    return out;
}
} // namespace p165w

TEST (glow_classic_measured_release_round165w)
{
    using namespace lynx;
    namespace g = mixsuite::glow;
    auto p = makeEffectSlot (EffectKind::glowLeveler).params;
    CHECK (p[g::releaseLaw] == 1.0f && p[g::voicing] == 1.0f && (int) effectInfo (EffectKind::glowLeveler).params.size() == g::count);
    // [EXEC] the trial (fresh instance: Gain 40, Peak reduction 46, Compress): 2 s at 0 dBFS, then the probe
    const std::vector<double> at { 0.1, 0.5, 1.0, 2.0, 5.0, 10.0 };
    static constexpr double trial46[] = { -6.20, -2.18, -1.29, -0.84, -0.31, -0.06 };
    auto got = p165w::glowRecovery (p, 0.0, 2.0, at);
    for (std::size_t k = 0; k < at.size(); ++k)
    {
        if (std::abs (got[k] - trial46[k]) >= 0.45)
            std::printf ("  glow 46: %.1f s %.2f dB (trial %.2f)\n", at[k], got[k], trial46[k]);
        CHECK (std::abs (got[k] - trial46[k]) < 0.45);
    }
    // the same after 10 s of reduction (the trial: no long memory, the same recovery)
    got = p165w::glowRecovery (p, 0.0, 10.0, at);
    for (std::size_t k = 0; k < at.size(); ++k)
        CHECK (std::abs (got[k] - trial46[k]) < 0.6);
    // Limit at peak reduction 62.6, 2 s at −6 dBFS
    p[g::mode] = 1.0f;
    p[g::peakReduction] = 0.626f;
    static constexpr double trialLimit[] = { -9.99, -3.03, -1.74, -1.12, -0.41, -0.08 };
    got = p165w::glowRecovery (p, -6.0, 2.0, at);
    for (std::size_t k = 0; k < at.size(); ++k)
    {
        if (std::abs (got[k] - trialLimit[k]) >= 0.7)
            std::printf ("  glow limit: %.1f s %.2f dB (trial %.2f)\n", at[k], got[k], trialLimit[k]);
        CHECK (std::abs (got[k] - trialLimit[k]) < 0.7);
    }
    // round 123's release (older projects) keeps its long tail: more than 0.5 dB still held 10 s after the burst
    p = makeEffectSlot (EffectKind::glowLeveler).params;
    p[g::releaseLaw] = 0.0f;
    got = p165w::glowRecovery (p, 0.0, 10.0, at);
    CHECK (got[5] < -0.5);
    CHECK (formatEffectParam (EffectKind::glowLeveler, g::releaseLaw, 0.0f, 120.0, p) == "round 123");
}

TEST (glow_older_projects_keep_the_round123_release_round165w)
{
    using namespace lynx;
    namespace g = mixsuite::glow;
    auto slot = makeEffectSlot (EffectKind::glowLeveler);
    upgradeLoadedEffectParams (slot, (std::size_t) g::releaseLaw);
    CHECK (slot.params[g::releaseLaw] == 0.0f && slot.params[g::optoLaw] == 1.0f);
    slot = makeEffectSlot (EffectKind::glowLeveler);
    upgradeLoadedEffectParams (slot, (std::size_t) g::count);
    CHECK (slot.params[g::releaseLaw] == 1.0f);
    // a CLA-2A state translates to the measured release
    int n = 0;
    std::string values;
    for (int i = 0; i < 208; ++i)
        values += i == 38 ? "40 " : i == 74 ? "46 " : i == 204 ? "100 " : i == 140 ? "2 " : "0 ";
    const auto s = vm123::wavesSlot ("CLA-2A", values.c_str(), n);
    CHECK (s.kind == EffectKind::glowLeveler && s.params[g::releaseLaw] == 1.0f);
}
