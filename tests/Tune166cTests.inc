// Round 166c: LYNX Tune's last auto-mode controls (docs/analise/REA_rodada166c.md): Throat length (law measured
// black-box in round 165hb), the created vibrato's Amplitude / Formant / Variation and Transpose in scale. Synthetic
// vowels and the independent pitch / formant measures of LynxTuneTests95.inc (r95::).

TEST (lynx_tune_round166c_value_laws)
{
    namespace t = lynx::lynxtune;
    // throat: 50 / 100 / 180 % at the ends and the middle, both halves invertible
    CHECK (std::abs (t::throatPercent (0.0f) - 50.0) < 1e-9 && std::abs (t::throatPercent (0.5f) - 100.0) < 1e-9 && std::abs (t::throatPercent (1.0f) - 180.0) < 1e-6);
    for (const double pc : { 50.0, 60.0, 72.0, 85.0, 100.0, 118.0, 135.0, 180.0 })
        CHECK (std::abs (t::throatPercent (t::throatValue (pc)) - pc) < 0.01);
    // the measured law (REA_rodada165hb §2, the mean of the 700 … 3000 Hz probes) within 1 semitone
    const std::pair<double, double> measured[] { { 50, 12.7 }, { 60, 9.7 }, { 72, 6.4 }, { 85, 3.0 }, { 118, -2.0 }, { 135, -4.5 }, { 180, -9.2 } };
    for (const auto& [pc, st] : measured)
        CHECK (std::abs (t::throatSemitones (pc) - st) < 1.0);
    CHECK (t::throatSemitones (100.0) == 0.0);
    // scale transpose: 15 steps, −7 … +7
    CHECK (t::scaleTransposeSteps (0.0f) == -7 && t::scaleTransposeSteps (0.5f) == 0 && t::scaleTransposeSteps (1.0f) == 7);
    for (int s = -7; s <= 7; ++s)
        CHECK (t::scaleTransposeSteps (t::scaleTransposeValue (s)) == s);
    CHECK (std::abs (t::vibratoAmplitudeOf (1.0f) - 30.0) < 1e-9 && std::abs (t::vibratoAmplitudeDepth (30.0) - 0.3) < 1e-9);
    // steps of the scale (C major): E4 + 2 = G4, B4 + 1 = C5, C4 − 3 = G3, A3 + 7 = A4; C#4 (out of the scale) + 1 = D#4
    const auto major = lynx::vocal::scaleMask (lynx::vocal::Scale::major, 0, 0xfff);
    CHECK (t::transposeInScaleNote (64, major, 2) == 67 && t::transposeInScaleNote (71, major, 1) == 72);
    CHECK (t::transposeInScaleNote (60, major, -3) == 55 && t::transposeInScaleNote (57, major, 7) == 69);
    CHECK (t::transposeInScaleNote (61, major, 1) == 63 && t::transposeInScaleNote (61, major, 0) == 61);
    // A minor: A3 + 2 = C4; chromatic: steps are semitones
    const auto minor = lynx::vocal::scaleMask (lynx::vocal::Scale::minor, 9, 0xfff);
    CHECK (t::transposeInScaleNote (57, minor, 2) == 60 && t::transposeInScaleNote (57, 0xfff, 2) == 59);
    // the new parameters are appended with neutral defaults (an old project keeps its sound)
    const auto& info = t::info();
    CHECK ((int) info.params.size() == t::count);
    CHECK (info.params[(std::size_t) t::throatLength].defaultValue == 0.5f && info.params[(std::size_t) t::scaleTranspose].defaultValue == 0.5f);
    CHECK (info.params[(std::size_t) t::vibratoAmplitude].defaultValue == 0.0f && info.params[(std::size_t) t::vibratoFormant].defaultValue == 0.0f);
    CHECK (info.params[(std::size_t) t::vibratoVariation].defaultValue == 0.0f && info.params[(std::size_t) t::transposeInScale].defaultValue == 0.0f);
    CHECK (t::paramText (t::throatLength, t::throatValue (72.0)) == "72%" && t::paramText (t::scaleTranspose, t::scaleTransposeValue (-3)) == "-3 steps");
    CHECK (t::paramText (t::transposeInScale, 1.0f) == "In scale" && t::paramText (t::vibratoAmplitude, 1.0f / 3.0f) == "10.0");
}

TEST (lynx_tune_round166c_throat_moves_the_formants)
{
    namespace t = lynx::lynxtune;
    const double fs = 44100.0;
    const auto x = r95::vowel (fs, (int) (fs * 1.5), [] (double) { return 164.8138; }); // E3, on its note
    auto p = r95::tuneDefaults();
    p[t::retune] = 0.0f;
    int latency = 0;
    const auto f1In = r95::formantCentre (x, 30000, 4096, fs);
    auto centre = [&] { return r95::formantCentre (r95::runTune (x, fs, p, latency), 30000 + latency, 4096, fs) / f1In; };
    const auto plain = centre();
    p[t::throatLength] = t::throatValue (72.0); // shorter: up (+6 st measured)
    const auto shorter = centre();
    p[t::throatLength] = t::throatValue (150.0); // longer: down
    const auto longer = centre();
    // with Formant preserve off the throat does nothing (as measured on the reference)
    p[t::formantPreserve] = 0.0f;
    const auto offLong = centre();
    p[t::throatLength] = 0.5f;
    const auto offPlain = centre();
    CHECK (std::abs (plain - 1.0) < 0.06);
    CHECK (shorter > 1.12);
    CHECK (longer < 0.9);
    CHECK (std::abs (offLong - offPlain) < 1e-6);
    // the pitch does not move
    p[t::formantPreserve] = 1.0f;
    p[t::throatLength] = t::throatValue (60.0);
    const auto y = r95::runTune (x, fs, p, latency);
    CHECK (std::abs (r95::cents (r95::measureHz (y, 30000 + latency, 2048, fs), 164.8138)) < 3.0);
}

TEST (lynx_tune_round166c_transpose_in_scale)
{
    namespace t = lynx::lynxtune;
    const double fs = 44100.0;
    // sung: A3 then E4 (on their notes), C major
    const auto x = r95::vowel (fs, (int) (fs * 1.6), [] (double s) { return s < 0.8 ? 220.0 : 329.6276; });
    auto p = r95::tuneDefaults();
    p[t::retune] = 0.0f;
    p[t::transposeInScale] = 1.0f;
    p[t::scaleTranspose] = t::scaleTransposeValue (2);
    p[t::transpose] = 17.0f / 24.0f; // +5: ignored while In scale is on
    int latency = 0;
    const auto y = r95::runTune (x, fs, p, latency);
    // A3 + 2 steps = C4 (a minor third), E4 + 2 steps = G4 (a minor third), never the knob's +5
    CHECK (std::abs (r95::cents (r95::measureHz (y, (int) (0.5 * fs) + latency, 2048, fs), 261.6256)) < 3.0);
    CHECK (std::abs (r95::cents (r95::measureHz (y, (int) (1.3 * fs) + latency, 2048, fs), 391.9954)) < 3.0);
    // three steps down: A3 → E3, E4 → B3
    p[t::scaleTranspose] = t::scaleTransposeValue (-3);
    const auto z = r95::runTune (x, fs, p, latency);
    CHECK (std::abs (r95::cents (r95::measureHz (z, (int) (0.5 * fs) + latency, 2048, fs), 164.8138)) < 3.0);
    CHECK (std::abs (r95::cents (r95::measureHz (z, (int) (1.3 * fs) + latency, 2048, fs), 246.9417)) < 3.0);
    // In scale off: the steps do nothing and the knob's semitones apply again
    p[t::transposeInScale] = 0.0f;
    const auto w = r95::runTune (x, fs, p, latency);
    CHECK (std::abs (r95::cents (r95::measureHz (w, (int) (0.5 * fs) + latency, 2048, fs), 220.0 * std::exp2 (5.0 / 12.0))) < 3.0);
}

TEST (lynx_tune_round166c_created_vibrato_amplitude_formant_variation)
{
    namespace t = lynx::lynxtune;
    const double fs = 44100.0;
    const auto x = r95::vowel (fs, (int) (fs * 2.0), [] (double) { return 220.0; }); // A3, on its note
    auto p = r95::tuneDefaults();
    p[t::retune] = 0.0f;
    p[t::vibratoShape] = 1.0f / 3.0f; // sine
    p[t::vibratoRate] = (float) ((2.0 - 0.1) / 9.9); // 2 Hz
    p[t::vibratoDelay] = 0.0f;
    p[t::vibratoOnset] = 0.0f;
    int latency = 0;
    // Amplitude: the level swings with the vibrato (30 = ±30 %), none at 0
    auto levelSwing = [&] {
        const auto y = r95::runTune (x, fs, p, latency);
        double lo = 1e9, hi = 0.0;
        for (double s = 0.5; s < 1.7; s += 0.01)
        {
            double e = 0.0;
            const auto a = (int) (s * fs);
            for (int i = a; i < a + 441; ++i)
                e += (double) y[(std::size_t) i] * y[(std::size_t) i];
            lo = std::min (lo, std::sqrt (e));
            hi = std::max (hi, std::sqrt (e));
        }
        return hi / lo;
    };
    p[t::vibratoDepth] = 0.0f; // the level swing alone
    const auto flat = levelSwing();
    p[t::vibratoAmplitude] = 1.0f;
    const auto swung = levelSwing();
    CHECK (flat < 1.1);
    CHECK (swung > 1.6 && swung < 2.2); // 1.3 / 0.7 = 1.86
    p[t::vibratoAmplitude] = 0.0f;

    // Formant: the formants follow the created vibrato's pitch by that share (formant preserve on)
    p[t::vibratoDepth] = 1.0f; // ±100 c
    auto formantSwing = [&] {
        const auto y = r95::runTune (x, fs, p, latency);
        double lo = 1e9, hi = 0.0;
        for (double s = 0.6; s < 1.6; s += 0.05)
        {
            const auto c = r95::formantCentre (y, (int) (s * fs) - 1024, 2048, fs);
            lo = std::min (lo, c);
            hi = std::max (hi, c);
        }
        return hi / lo;
    };
    const auto kept = formantSwing();
    p[t::vibratoFormant] = 1.0f;
    const auto followed = formantSwing();
    CHECK (followed > kept + 0.025); // (measured: 1.039 → 1.081; the centre of 200 … 1800 Hz is dominated by the harmonics)

    // Variation: the cycles drift (the output differs), the depth stays bounded (≤ 1.5 × 100 c + slack)
    p[t::vibratoFormant] = 0.0f;
    const auto steady = r95::runTune (x, fs, p, latency);
    p[t::vibratoVariation] = 1.0f;
    const auto varied = r95::runTune (x, fs, p, latency);
    double diff = 0.0, ref = 0.0, worst = 0.0;
    for (std::size_t i = (std::size_t) fs / 2; i < steady.size(); ++i)
    {
        diff += (double) (varied[i] - steady[i]) * (varied[i] - steady[i]);
        ref += (double) steady[i] * steady[i];
    }
    for (double s = 0.5; s < 1.9; s += 0.02)
        worst = std::max (worst, std::abs (r95::cents (r95::measureHz (varied, (int) (s * fs), 1024, fs, 150.0, 400.0), 220.0)));
    CHECK (diff / ref > 0.05);
    CHECK (worst < 160.0);
    // and it is deterministic (same input, same output)
    const auto again = r95::runTune (x, fs, p, latency);
    CHECK (again == varied);

    // real time: no allocation with every new control on
    p[t::vibratoAmplitude] = 1.0f;
    p[t::vibratoFormant] = 1.0f;
    p[t::throatLength] = 0.2f;
    p[t::transposeInScale] = 1.0f;
    p[t::scaleTranspose] = 0.8f;
    auto fx = lynx::makeEffect (lynx::EffectKind::lynxTune);
    fx->prepare (fs);
    std::vector<float> l = x, r = x;
    rt95::allocs = 0;
    rt95::countAllocs = true;
    for (int i = 0; i + 256 <= (int) x.size(); i += 256)
        fx->process (l.data() + i, r.data() + i, 256, p, 120.0);
    rt95::countAllocs = false;
    CHECK (rt95::allocs.load() == 0);
    CHECK (std::all_of (l.begin(), l.end(), [] (float v) { return std::isfinite (v) && std::abs (v) < 4.0f; }));
}

TEST (lynx_tune_round166c_auto_tune_pro_controls_translate)
{
    using namespace lynx;
    namespace t = lynxtune;
    // Throat 72 %, created vibrato Amplitude 20 / Formant 50 / Variation 40, Transpose In Scale with −3 steps
    int n = 0;
    const auto s = vm127::tuneSlot (vm127::autoTuneState (0, 0, { { 70, 1 }, { 71, 72 }, { 47, 20 }, { 48, 50 }, { 49, 40 }, { 179, 1 }, { 181, -3 } }), n);
    CHECK (std::abs (t::throatPercent (s.params[t::throatLength]) - 72.0) < 0.01 && std::abs (t::formantSemitones (s.params[t::formantShift])) < 1e-4);
    CHECK (std::abs (t::vibratoAmplitudeOf (s.params[t::vibratoAmplitude]) - 20.0) < 1e-3);
    CHECK (std::abs (s.params[t::vibratoFormant] - 0.5f) < 1e-6f && std::abs (s.params[t::vibratoVariation] - 0.4f) < 1e-6f);
    CHECK (s.params[t::transposeInScale] == 1.0f && t::scaleTransposeSteps (s.params[t::scaleTranspose]) == -3);
    // Classic mode: no throat, no transpose in scale (the reference disables them)
    const auto c = vm127::tuneSlot (vm127::autoTuneState (0, 0, { { 11, 1 }, { 71, 150 }, { 179, 1 } }), n);
    CHECK (c.params[t::throatLength] == 0.5f && c.params[t::transposeInScale] == 0.0f);
}
