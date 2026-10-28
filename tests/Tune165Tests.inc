// Round 165j: LYNX Tune against Auto-Tune Pro / Waves Tune Real-Time on her takes and synthetic voices
// (docs/analise/REA_rodada165j.md §4): the corrected voice kept its highs, and the grain overlap-add is normalised.

namespace tune165
{
// energy above ~2.5 kHz relative to the energy under ~1 kHz (dB), from first-order filters, on [from, to)
double highOverLow (const std::vector<float>& x, int from, int to, double fs)
{
    const double ah = std::exp (-2.0 * 3.14159265358979 * 2500.0 / fs), al = 1.0 - std::exp (-2.0 * 3.14159265358979 * 1000.0 / fs);
    double hx = 0.0, hy = 0.0, l = 0.0, eh = 0.0, el = 0.0;
    for (int i = 0; i < to && i < (int) x.size(); ++i)
    {
        const double v = x[(std::size_t) i];
        hy = ah * (hy + v - hx);
        hx = v;
        l += al * (v - l);
        if (i >= from)
        {
            eh += hy * hy;
            el += l * l;
        }
    }
    return 10.0 * std::log10 ((eh + 1e-20) / (el + 1e-20));
}
} // namespace tune165

TEST (tune_keeps_the_highs_of_a_corrected_vowel_round165j)
{
    // A3 sung 40 c sharp and D4 45 c flat (chromatic, default retune): the corrected vowel's high / low balance stays
    // within 0.5 dB of the sung one (a guard: round 165j measured her corrected takes 0.5 dB darker at 2.5-6 kHz before
    // the normalised overlap-add and the 3 kHz shelf, REA_rodada165j §4)
    const double fs = 44100.0;
    for (const double cents : { 40.0, -45.0 })
    {
        const double hz = (cents > 0 ? 220.0 : 293.66) * std::exp2 (cents / 1200.0);
        const auto x = r95::vowel (fs, (int) (fs * 1.2), [hz] (double) { return hz; });
        auto p = r95::tuneDefaults();
        p[lynx::lynxtune::scale] = 0.0f;
        int latency = 0;
        const auto y = r95::runTune (x, fs, p, latency);
        const int a = (int) (0.4 * fs), b = (int) (1.1 * fs);
        const auto dIn = tune165::highOverLow (x, a, b, fs), dOut = tune165::highOverLow (y, a + latency, b + latency, fs);
        CHECK (std::abs (dOut - dIn) < 0.5);
    }
}

TEST (psola_normalised_overlap_add_round165j)
{
    // the normalised overlap-add keeps the level of a steady vowel within 0.25 dB at the usual corrections (−40 c, +60 c),
    // and unshifted it is as loud as the plain overlap-add (within 0.1 dB)
    const double fs = 44100.0;
    const auto x = r95::vowel (fs, (int) (fs * 1.0), [] (double) { return 220.0; });
    for (const double cents : { 0.0, -40.0, 60.0 })
    {
        double e[2] = { 0.0, 0.0 }, ex = 0.0;
        for (int k = 0; k < 2; ++k)
        {
            lynx::vocal::Psola s;
            s.prepare (fs, fs / 80.0, 1200);
            s.setNormalise (k == 1);
            for (int i = 0; i < (int) x.size(); ++i)
            {
                s.setPitch (fs / 220.0, true);
                s.setShift (std::exp2 (cents / 1200.0), 1.0);
                float y = 0.0f;
                s.process (&x[(std::size_t) i], &y, 1);
                if (i >= (int) (0.3 * fs) && i < (int) (0.9 * fs))
                {
                    e[k] += (double) y * y;
                    if (k == 0)
                        ex += (double) x[(std::size_t) (i - 1200)] * x[(std::size_t) (i - 1200)];
                }
            }
        }
        CHECK (std::abs (10.0 * std::log10 (e[1] / ex)) < 0.25);
        if (cents == 0.0)
            CHECK (std::abs (10.0 * std::log10 (e[1] / e[0])) < 0.1);
    }
}
