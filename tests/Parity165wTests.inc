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
        // the editor's curve (the analog prototype) agrees
        CHECK (std::abs (g::shelfResponseDb (pt.hz, pt.f0, pt.db, pt.high, true) - pt.want) < 0.4);
    }
    // round 124's law stays for projects saved before (one first-order section, −30 … +12)
    {
        auto p = base;
        p[g::eqLaw] = 0.0f;
        p[g::highOn] = 1.0f;
        p[g::highGain] = g::shelfValue (-12.0, false);
        p[g::highFreq] = g::freqValue (2448.0);
        const auto got = level (p, 7962.0) - level (base, 7962.0);
        CHECK (std::abs (got - g::shelfResponseDb (7962.0, 2448.0, -12.0, true, false)) < 0.4);
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
