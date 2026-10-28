// --- round 128: parity with the originals (docs/PARITY.md) — Facet EQ's Pro-Q controls --------------------------------

namespace parity128
{
using namespace lynx::facet;

// a stereo test signal: different sines left and right, `secs` long; processed in 480-sample blocks
struct Stereo
{
    std::vector<float> l, r;
};

Stereo tones (double fl, double fr, double secs, double amp = 0.25)
{
    const double sr = 48000.0;
    const int n = (int) (secs * sr);
    Stereo s { std::vector<float> ((std::size_t) n), std::vector<float> ((std::size_t) n) };
    for (int i = 0; i < n; ++i)
    {
        s.l[(std::size_t) i] = (float) (amp * std::sin (2.0 * 3.14159265358979 * fl * i / sr));
        s.r[(std::size_t) i] = (float) (amp * std::sin (2.0 * 3.14159265358979 * fr * i / sr + 0.3));
    }
    return s;
}

Stereo run (const lynx::EffectParams& p, Stereo s)
{
    auto e = make (&p);
    e->prepare (48000.0);
    for (std::size_t o = 0; o < s.l.size(); o += 480)
    {
        const int m = (int) std::min<std::size_t> (480, s.l.size() - o);
        e->process (s.l.data() + o, s.r.data() + o, m, p, 120.0);
    }
    return s;
}

double maxDiff (const std::vector<float>& a, const std::vector<float>& b, std::size_t from = 0)
{
    double d = 0.0;
    for (std::size_t i = from; i < a.size() && i < b.size(); ++i) d = std::max (d, (double) std::abs (a[i] - b[i]));
    return d;
}

double rms (const std::vector<float>& a, std::size_t from)
{
    double s = 0.0;
    for (std::size_t i = from; i < a.size(); ++i) s += (double) a[i] * a[i];
    return std::sqrt (s / (double) std::max<std::size_t> (1, a.size() - from));
}
} // namespace parity128

TEST (facet_round128_parameters_default_to_the_old_behaviour)
{
    using namespace lynx::facet;
    const auto p = facet103::defaults();
    CHECK (info().params.size() == (std::size_t) paramCount && paramCount >= 88 && characterParam == 88 && paramCount <= lynx::maxEffectParams); // round 135 appended Character
    for (int b = 0; b < bands; ++b)
    {
        CHECK (bandPower (p, b));
        CHECK (placement (p[(std::size_t) (placementParam0 + b)]) == Placement::stereo);
    }
    CHECK (p[(std::size_t) autoGainParam] < 0.5f && p[(std::size_t) invertParam] < 0.5f);
    CHECK (std::abs (panAmount (p[(std::size_t) panParam])) < 1e-9 && ! panMidSide (p[(std::size_t) panModeParam]));
    CHECK (analyzerRangeDb (p[(std::size_t) specRangeParam]) == 90.0 && analyzerSpeed (p[(std::size_t) specSpeedParam]) == 2);
    CHECK (std::abs (analyzerTiltDb (p[(std::size_t) specTiltParam]) - 4.5) < 1e-9 && displayRangeDb (p[(std::size_t) displayRangeParam]) == 12.0);
    for (const auto db : { 3.0, 6.0, 12.0, 30.0 }) CHECK (displayRangeDb (displayRangeValue (db)) == db);
    for (int k = 0; k < placementCount; ++k) CHECK ((int) placement (placementValue ((Placement) k)) == k);
    // a bell with every new control at its default: the same samples as the bell alone (old projects sound the same)
    auto bellOnly = facet103::bell (1000.0, 9.0, 1.0);
    auto explicitDefaults = bellOnly;
    for (int b = 0; b < bands; ++b) { explicitDefaults[(std::size_t) (powerParam0 + b)] = 0.0f; explicitDefaults[(std::size_t) (placementParam0 + b)] = 0.0f; }
    const auto in = parity128::tones (700.0, 1300.0, 0.3);
    CHECK (parity128::maxDiff (parity128::run (bellOnly, in).l, parity128::run (explicitDefaults, in).l) == 0.0);
    CHECK (lynx::formatEffectParam (lynx::EffectKind::facetEq, placementParam0, placementValue (Placement::mid), 120.0) == "Mid");
    CHECK (lynx::formatEffectParam (lynx::EffectKind::facetEq, powerParam0 + 2, 1.0f, 120.0) == "Off");
    CHECK (lynx::formatEffectParam (lynx::EffectKind::facetEq, panParam, 0.25f, 120.0) == "L 50%");
}

TEST (facet_round128_band_power_keeps_settings_and_bypasses_the_band)
{
    using namespace lynx::facet;
    const auto in = parity128::tones (1000.0, 1000.0, 0.3);
    auto off = facet103::bell (1000.0, 12.0, 1.0);
    off[(std::size_t) powerParam0] = 1.0f;
    const auto out = parity128::run (off, in);
    CHECK (parity128::maxDiff (out.l, in.l) < 1e-6 && parity128::maxDiff (out.r, in.r) < 1e-6); // flat
    CHECK (std::abs (bandResponseDb (off, 0, 48000.0, 1000.0)) < 1e-9 && ! isDynamic (off, 0));
    auto dyn = off;
    dyn[(std::size_t) rangeParam0] = rangeValue (-6.0);
    CHECK (! isDynamic (dyn, 0));
    // switched back on: the band's settings are still there
    off[(std::size_t) powerParam0] = 0.0f;
    CHECK (std::abs (bandResponseDb (off, 0, 48000.0, 1000.0) - 12.0) < 0.05);
}

TEST (facet_round128_stereo_placement_left_right_mid_side)
{
    using namespace lynx::facet;
    const auto in = parity128::tones (1000.0, 1000.0, 0.4);
    auto p = facet103::bell (1000.0, 12.0, 1.0);
    const auto stereo = parity128::run (p, in);
    // left: the right channel passes untouched, the left one gets the band
    p[(std::size_t) placementParam0] = placementValue (Placement::left);
    auto out = parity128::run (p, in);
    CHECK (parity128::maxDiff (out.r, in.r) < 1e-6);
    CHECK (parity128::maxDiff (out.l, stereo.l) < 1e-6);
    p[(std::size_t) placementParam0] = placementValue (Placement::right);
    out = parity128::run (p, in);
    CHECK (parity128::maxDiff (out.l, in.l) < 1e-6 && parity128::maxDiff (out.r, stereo.r) < 1e-6);
    // mid: a pure side signal (L = −R) is untouched; side: a pure mid signal (L = R) is untouched
    auto side = parity128::tones (1000.0, 1000.0, 0.4);
    for (std::size_t i = 0; i < side.r.size(); ++i) side.r[i] = -side.l[i];
    auto mid = parity128::tones (1000.0, 1000.0, 0.4);
    for (std::size_t i = 0; i < mid.r.size(); ++i) mid.r[i] = mid.l[i];
    p[(std::size_t) placementParam0] = placementValue (Placement::mid);
    out = parity128::run (p, side);
    CHECK (parity128::maxDiff (out.l, side.l) < 1e-6 && parity128::maxDiff (out.r, side.r) < 1e-6);
    out = parity128::run (p, mid);
    const auto from = (std::size_t) (0.2 * 48000.0);
    CHECK (std::abs (20.0 * std::log10 (parity128::rms (out.l, from) / parity128::rms (mid.l, from)) - 12.0) < 0.2);
    p[(std::size_t) placementParam0] = placementValue (Placement::side);
    out = parity128::run (p, mid);
    CHECK (parity128::maxDiff (out.l, mid.l) < 1e-6 && parity128::maxDiff (out.r, mid.r) < 1e-6);
    // in the FIR modes a placed band runs as sections after the (stereo) FIR: the other channel is the delayed input
    p[(std::size_t) placementParam0] = placementValue (Placement::left);
    p[(std::size_t) phaseParam] = phaseValue (Phase::natural);
    out = parity128::run (p, in);
    const auto lat = (std::size_t) latencyFor (Phase::natural, 48000.0);
    double d = 0.0;
    for (std::size_t i = from; i < out.r.size(); ++i) d = std::max (d, (double) std::abs (out.r[i] - in.r[i - lat]));
    CHECK (d < 1e-4);
    CHECK (std::abs (20.0 * std::log10 (parity128::rms (out.l, from) / parity128::rms (in.l, from)) - 12.0) < 0.3);
}

TEST (facet_round128_output_auto_gain_polarity_pan)
{
    using namespace lynx::facet;
    const auto in = parity128::tones (1000.0, 1000.0, 0.3);
    auto p = facet103::defaults();
    p[(std::size_t) param (0, 0)] = shapeValue (Shape::highShelf);
    p[(std::size_t) param (0, 1)] = hzValue (2000.0);
    p[(std::size_t) param (0, 2)] = gainValue (6.0);
    // auto gain takes back the curve's mean level change (a +6 dB shelf above 2 kHz: about a third of the log band)
    const auto ag = autoGainDb (p, 48000.0);
    CHECK (ag > 1.0 && ag < 4.0);
    const auto plain = parity128::run (p, in);
    p[(std::size_t) autoGainParam] = 1.0f;
    const auto comp = parity128::run (p, in);
    const auto from = (std::size_t) (0.1 * 48000.0);
    CHECK (std::abs (20.0 * std::log10 (parity128::rms (plain.l, from) / parity128::rms (comp.l, from)) - ag) < 0.05);
    p[(std::size_t) autoGainParam] = 0.0f;
    // polarity
    p[(std::size_t) invertParam] = 1.0f;
    const auto inv = parity128::run (p, in);
    double d = 0.0;
    for (std::size_t i = 0; i < inv.l.size(); ++i) d = std::max (d, (double) std::abs (inv.l[i] + plain.l[i]));
    CHECK (d < 1e-6);
    p[(std::size_t) invertParam] = 0.0f;
    // pan fully right: the left channel is silent, the right one unchanged
    p[(std::size_t) panParam] = 1.0f;
    const auto right = parity128::run (p, in);
    CHECK (parity128::rms (right.l, 0) < 1e-9 && parity128::maxDiff (right.r, plain.r) < 1e-6);
    // mid / side mode, fully "side": a pure mid input disappears
    auto mid = in;
    for (std::size_t i = 0; i < mid.r.size(); ++i) mid.r[i] = mid.l[i];
    p[(std::size_t) panModeParam] = 1.0f;
    const auto ms = parity128::run (p, mid);
    CHECK (parity128::rms (ms.l, from) < 1e-6 && parity128::rms (ms.r, from) < 1e-6);
}

// --- round 128: Ember Saturator's Saturn controls (input, band power / solo / mute, tone, feedback, dynamics) --------

TEST (ember_round128_new_parameters_default_to_the_old_sound)
{
    using namespace lynx::mixsuite::ember;
    const auto& in = lynx::effectInfo (lynx::EffectKind::ember);
    CHECK ((int) in.params.size() == count && count == 160 && count <= lynx::maxEffectParams); // round 135 (7) + round 136 (the Classic voicing, 11) + round 147b (9) + round 150b (41) + round 152b (25) + round 165a (2)
    const auto p = mixsuite96::defaults (lynx::EffectKind::ember);
    CHECK (std::abs (inputDb (p[inputGain])) < 1e-9);
    for (int b = 0; b < maxBands; ++b)
    {
        CHECK (p[(std::size_t) bandExtra (b, 0)] >= 0.5f && bandState (p[(std::size_t) bandExtra (b, 1)]) == BandState::normal);
        for (int k = 2; k < 6; ++k) CHECK (std::abs (toneDb (p[(std::size_t) bandExtra (b, k)])) < 1e-9);
        CHECK (feedbackAmount (p[(std::size_t) bandExtra (b, 6)]) == 0.0 && std::abs (feedbackHz (p[(std::size_t) bandExtra (b, 7)]) - 250.0) < 1.0);
        CHECK (dynamicsAmount (p[(std::size_t) bandExtra (b, 8)]) == 0.0);
        CHECK (std::string (in.params[(std::size_t) bandExtra (b, 0)].name) == "B" + std::to_string (b + 1) + " on");
    }
    CHECK (lynx::formatEffectParam (lynx::EffectKind::ember, bandExtra (1, 1), 1.0f, 120.0) == "mute");
    CHECK (lynx::formatEffectParam (lynx::EffectKind::ember, bandExtra (0, 2), 1.0f, 120.0) == "+18.0 dB");
}

TEST (ember_round128_band_power_mute_solo)
{
    using namespace lynx::mixsuite::ember;
    using namespace mixsuite96;
    const auto x = sine (220.0, 0.3, 16384);
    auto p = defaults (lynx::EffectKind::ember);
    p[drive] = 0.7f;
    // switched off: the input comes out clean (delayed by the oversampler's latency)
    p[(std::size_t) bandExtra (0, 0)] = 0.0f;
    const auto y = run (lynx::EffectKind::ember, p, x);
    const auto lat = (std::size_t) lynx::mixsuite::Oversampler::latency();
    double d = 0.0;
    for (std::size_t i = 4096; i < y.size(); ++i) d = std::max (d, (double) std::abs (y[i] - x[i - lat]));
    CHECK (d < 1e-6);
    // muted: silence (mix 100 %)
    p[(std::size_t) bandExtra (0, 0)] = 1.0f;
    p[(std::size_t) bandExtra (0, 1)] = 1.0f;
    const auto silent = run (lynx::EffectKind::ember, p, x);
    CHECK (amplitude (silent, 220.0, 8192, 8192) < 1e-6);
    // two bands split at 1 kHz, the high one soloed: a 220 Hz tone (low band) nearly disappears
    auto m = defaults (lynx::EffectKind::ember);
    m[bands] = 1.0f / 3.0f;
    m[xover1] = crossoverValue (1000.0);
    const auto both = amplitude (run (lynx::EffectKind::ember, m, x), 220.0, 8192, 8192);
    m[(std::size_t) bandExtra (1, 1)] = 0.5f; // solo
    const auto solo = amplitude (run (lynx::EffectKind::ember, m, x), 220.0, 8192, 8192);
    CHECK (db (solo / both) < -20.0);
}

TEST (ember_round128_tone_feedback_dynamics_input)
{
    using namespace lynx::mixsuite::ember;
    using namespace mixsuite96;
    // clean drive (0 dB) on a quiet tone: the bass control moves a 60 Hz tone by its gain, a 3 kHz one hardly
    auto p = defaults (lynx::EffectKind::ember);
    const auto low = sine (60.0, 0.01, 16384), high = sine (3000.0, 0.01, 16384);
    const auto a0 = amplitude (run (lynx::EffectKind::ember, p, low), 60.0, 8192, 8192);
    const auto h0 = amplitude (run (lynx::EffectKind::ember, p, high), 3000.0, 8192, 8192);
    p[(std::size_t) bandExtra (0, 2)] = 5.0f / 6.0f; // bass +12 dB
    const auto a1 = amplitude (run (lynx::EffectKind::ember, p, low), 60.0, 8192, 8192);
    const auto h1 = amplitude (run (lynx::EffectKind::ember, p, high), 3000.0, 8192, 8192);
    std::printf ("  Ember bass +12 dB: 60 Hz %+.2f dB, 3 kHz %+.2f dB\n", db (a1 / a0), db (h1 / h0));
    CHECK (db (a1 / a0) > 10.0 && db (a1 / a0) < 12.5 && std::abs (db (h1 / h0)) < 0.5);
    // presence +12 dB lifts 6 kHz
    p[(std::size_t) bandExtra (0, 2)] = 0.5f;
    p[(std::size_t) bandExtra (0, 5)] = 5.0f / 6.0f;
    const auto pres = amplitude (run (lynx::EffectKind::ember, p, sine (6000.0, 0.01, 16384)), 6000.0, 8192, 8192);
    const auto pres0 = amplitude (run (lynx::EffectKind::ember, defaults (lynx::EffectKind::ember), sine (6000.0, 0.01, 16384)), 6000.0, 8192, 8192);
    CHECK (db (pres / pres0) > 10.0);
    // input gain +12 dB: a clean tone 12 dB louder (auto gain only follows the drive)
    auto q = defaults (lynx::EffectKind::ember);
    q[inputGain] = 0.75f;
    CHECK (std::abs (db (amplitude (run (lynx::EffectKind::ember, q, low), 60.0, 8192, 8192) / a0) - 12.0) < 0.3);
    // dynamics: expansion makes a loud tone louder than a soft one would be, compression the opposite
    auto dyn = [] (float amount, double amp) {
        auto r = defaults (lynx::EffectKind::ember);
        r[(std::size_t) bandExtra (0, 8)] = amount;
        return amplitude (run (lynx::EffectKind::ember, r, sine (500.0, amp, 16384)), 500.0, 8192, 8192) / amp;
    };
    const auto flat = dyn (0.5f, 0.5), expand = dyn (1.0f, 0.5), compress = dyn (0.0f, 0.5);
    CHECK (expand > flat * 1.5 && compress < flat / 1.5);
    // feedback: changes the sound and stays bounded at its maximum
    auto f = defaults (lynx::EffectKind::ember);
    f[drive] = 0.5f;
    const auto plain = run (lynx::EffectKind::ember, f, sine (150.0, 0.5, 16384));
    f[(std::size_t) bandExtra (0, 6)] = 1.0f;
    f[(std::size_t) bandExtra (0, 7)] = 0.5f; // 100 Hz
    const auto fed = run (lynx::EffectKind::ember, f, sine (150.0, 0.5, 16384));
    double diff = 0.0, peak = 0.0;
    for (std::size_t i = 0; i < fed.size(); ++i)
    {
        CHECK (std::isfinite (fed[i]));
        diff = std::max (diff, (double) std::abs (fed[i] - plain[i]));
        peak = std::max (peak, (double) std::abs (fed[i]));
    }
    CHECK (diff > 0.01 && peak < 4.0);
}

TEST (ember_round128_saturn2_tone_and_state_translated)
{
    using namespace lynx;
    PluginIdentity id;
    id.name = "FabFilter Saturn 2";
    // one band: bass +6 dB, presence −3 dB, feedback 40 % at 200 Hz, dynamics +0.5, muted; input +3 dB
    id.state = dyn108::ffbs (951, { { 0, 3.0f }, { 5, 100.0f }, { 6, 1.0f }, { 7 + 0, 40.0f }, { 7 + 1, 200.0f }, { 7 + 2, 0.5f }, { 7 + 3, 6.0f },
                                    { 7 + 6, 6.0f }, { 7 + 9, -3.0f }, { 7 + 10, 100.0f }, { 7 + 13, 1.0f }, { 7 + 14, 2.0f } });
    const auto* entry = findPluginSubstitute (id, false);
    CHECK (entry != nullptr && entry->effect == EffectKind::ember);
    if (entry == nullptr) return;
    const auto slot = makeSubstituteSlot (*entry, &id);
    namespace e = mixsuite::ember;
    CHECK (std::abs (e::inputDb (slot.params[e::inputGain]) - 3.0) < 0.01);
    CHECK (std::abs (e::toneDb (slot.params[(std::size_t) e::bandExtra (0, 2)]) - 6.0) < 0.01);
    CHECK (std::abs (e::toneDb (slot.params[(std::size_t) e::bandExtra (0, 5)]) + 3.0) < 0.01);
    CHECK (std::abs (e::feedbackAmount (slot.params[(std::size_t) e::bandExtra (0, 6)]) - 0.4) < 1e-4);
    CHECK (std::abs (e::feedbackHz (slot.params[(std::size_t) e::bandExtra (0, 7)]) - 200.0) < 0.5);
    CHECK (std::abs (e::dynamicsAmount (slot.params[(std::size_t) e::bandExtra (0, 8)]) - 0.5) < 1e-4);
    CHECK (e::bandState (slot.params[(std::size_t) e::bandExtra (0, 1)]) == e::BandState::mute);
}
