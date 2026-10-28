// --- round 128 (parity128a): Apex Limiter and Hush De-esser controls added for parity with the reference limiter and
// de-esser (docs/PARITY.md). Every new parameter is appended and its default leaves the output unchanged (checked
// against the round-127 engines by tmp/parity128 harness: byte-identical over 3 rates × 64 settings).

#include "core/ApexLimiter.h"
#include "core/MixSuite.h"

namespace parity128a
{
inline lynx::EffectParams defaults (lynx::EffectKind kind) { return lynx::makeEffectSlot (kind).params; }

// stereo test signal: a loud burst (0.25 s) then quiet, left and right different
inline void signal (std::vector<float>& l, std::vector<float>& r, int n, double sr, double loud = 0.9, double quiet = 0.05)
{
    l.assign ((std::size_t) n, 0.0f);
    r.assign ((std::size_t) n, 0.0f);
    for (int i = 0; i < n; ++i)
    {
        const auto a = (i % (int) sr) < (int) (0.25 * sr) ? loud : quiet;
        l[(std::size_t) i] = (float) (a * std::sin (2.0 * M_PI * 110.0 * i / sr));
        r[(std::size_t) i] = (float) (a * 0.7 * std::sin (2.0 * M_PI * 165.0 * i / sr + 0.4));
    }
}

inline void process (lynx::Effect& fx, std::vector<float>& l, std::vector<float>& r, const lynx::EffectParams& p, int block = 256)
{
    const auto n = (int) l.size();
    for (int i = 0; i < n; i += block)
        fx.process (l.data() + i, r.data() + i, std::min (block, n - i), p, 120.0);
}

inline double rms (const std::vector<float>& x, std::size_t a, std::size_t b)
{
    double s = 0.0;
    for (auto i = a; i < b; ++i) s += (double) x[i] * x[i];
    return std::sqrt (s / (double) std::max<std::size_t> (1, b - a));
}
} // namespace parity128a

TEST (parity128a_apex_new_controls)
{
    using namespace lynx::apex;
    using namespace parity128a;
    const double sr = 48000.0;
    // 8 styles; the old 4-step values keep their style
    CHECK (styleCount == 8 && info().params.size() == (std::size_t) paramCount && pMeterScale == 14 && paramCount >= 15); // round 135 appended more
    CHECK (std::string (styleName (style (0.0f))) == "Clean" && std::string (styleName (style (0.3333f))) == "Punch");
    CHECK (std::string (styleName (style (0.6667f))) == "Loud" && std::string (styleName (style (1.0f))) == "Safe");
    CHECK (paramText (pAttack, 0.0f) == "0 ms" && paramText (pAttack, 0.5f) == "625 ms" && paramText (pAttack, 1.0f) == "10.00 s");
    CHECK (paramText (pDither, 0.0f) == "Off" && paramText (pDither, 0.2f) == "16 bits" && paramText (pShaping, 2.0f / 3.0f) == "Optimized");
    CHECK (paramText (pMeterScale, 0.4f) == "-48 dB" && paramText (pMeterScale, 1.0f) == "K-20");

    auto p = defaults (lynx::EffectKind::apexLimiter);
    CHECK (p[pAttack] == 0.0f && p[pLinkRelease] == 0.0f && p[pDither] == 0.0f && p[pUnity] == 0.0f && p[pAudition] == 0.0f && p[pDcFilter] == 0.0f);
    p[pGain] = 12.0f / 30.0f;
    p[pRelease] = releaseValue (300.0);
    const int n = (int) sr * 2;
    std::vector<float> l0, r0;
    signal (l0, r0, n, sr);

    // attack: a slower slow stage dips less, so the quiet part after the burst comes back sooner (louder)
    auto run = [&] (const lynx::EffectParams& q, std::vector<float>& l, std::vector<float>& r) {
        auto fx = make();
        fx->prepare (sr);
        l = l0;
        r = r0;
        process (*fx, l, r, q);
    };
    std::vector<float> la, ra, lb, rb;
    p[pStyle] = 1.0f; // Safe: the most slow stage
    run (p, la, ra);
    auto q = p;
    q[pAttack] = 0.6f; // ≈ 1.3 s
    run (q, lb, rb);
    const auto after0 = rms (la, (std::size_t) (0.3 * sr), (std::size_t) (0.5 * sr)), after1 = rms (lb, (std::size_t) (0.3 * sr), (std::size_t) (0.5 * sr));
    std::printf ("  apex attack: quiet part after the burst %.4f -> %.4f\n", after0, after1);
    CHECK (after1 > after0 * 1.02);

    // dither: every output sample on the 16-bit grid
    q = p;
    q[pDither] = 0.2f;
    run (q, lb, rb);
    bool grid = true;
    for (int i = 1000; i < n; i += 7) grid = grid && std::abs (lb[(std::size_t) i] * 32768.0 - std::round (lb[(std::size_t) i] * 32768.0)) < 1e-3;
    CHECK (grid);

    // unity gain: below the ceiling the output is the input (delayed), not the input + gain
    q = p;
    q[pUnity] = 1.0f;
    q[pGain] = 6.0f / 30.0f;
    {
        auto fx = make();
        fx->prepare (sr);
        std::vector<float> l (8192), r (8192);
        for (int i = 0; i < 8192; ++i) l[(std::size_t) i] = r[(std::size_t) i] = (float) (0.1 * std::sin (i * 0.05));
        const auto in = l;
        process (*fx, l, r, q);
        const auto lat = latencyAt (sr);
        double err = 0.0;
        for (int i = lat; i < 8192; ++i) err = std::max (err, (double) std::abs (l[(std::size_t) i] - in[(std::size_t) (i - lat)]));
        CHECK (err < 1e-5);
    }

    // audition: what is removed = (input after gain, delayed) − output; silent when nothing is limited
    q = p;
    q[pAudition] = 1.0f;
    run (q, lb, rb);
    run (p, la, ra);
    CHECK (rms (lb, (std::size_t) (0.05 * sr), (std::size_t) (0.25 * sr)) > 0.05); // the burst is limited
    {
        // output + what is removed = the input after the gain (delayed by the latency)
        const auto lat = (std::size_t) latencyAt (sr);
        const auto g = std::pow (10.0, 12.0 / 20.0);
        double err = 0.0;
        for (auto i = lat; i < (std::size_t) n; ++i) err = std::max (err, std::abs ((double) la[i] + lb[i] - g * l0[i - lat]));
        std::printf ("  apex audition: output + removed vs input x gain: %.2g\n", err);
        CHECK (err < 1e-4);
    }

    // release link 0 with transient link 0: the channels are limited independently (right, quieter, keeps more level)
    q = p;
    q[pLink] = 0.0f;
    q[pLinkRelease] = 0.0f;
    run (q, lb, rb);
    q[pLinkRelease] = 1.0f;
    std::vector<float> lc, rc;
    run (q, lc, rc);
    const auto rFree = rms (rb, (std::size_t) (0.3 * sr), (std::size_t) (0.5 * sr)), rLinked = rms (rc, (std::size_t) (0.3 * sr), (std::size_t) (0.5 * sr));
    std::printf ("  apex release link: right after the burst %.4f (free) %.4f (linked)\n", rFree, rLinked);
    CHECK (rLinked < rFree);

    // DC filter: a DC offset is removed
    q = p;
    q[pGain] = 0.0f;
    q[pDcFilter] = 1.0f;
    {
        auto fx = make();
        fx->prepare (sr);
        std::vector<float> l ((std::size_t) n, 0.2f), r ((std::size_t) n, 0.2f);
        process (*fx, l, r, q);
        CHECK (std::abs (l[(std::size_t) n - 1]) < 0.005f);
    }
}

TEST (parity128a_hush_new_controls)
{
    using namespace lynx::mixsuite::hush;
    using namespace parity128a;
    using lynx::EffectKind;
    const double sr = 44100.0;
    const int n = 44100;
    CHECK (lynx::mixsuite::hushInfo().params.size() == (std::size_t) count && display == 17 && count >= 18); // round 135 appended Oversampling
    auto p = defaults (EffectKind::hushDeEsser);
    CHECK (p[allround] == 0.0f && p[lowPass] == 1.0f && std::abs (lookaheadTimeMs (p[lookaheadTime]) - 2.0) < 1e-6 && p[stereoLink] == 1.0f && p[input] == 0.5f);
    p[threshold] = (float) (20.0 / 60.0); // −40 dBFS
    p[range] = 0.5f;                      // 12 dB
    p[frequency] = 0.5f * (float) (std::log (2.0) / std::log (8.0)); // 2.83 kHz
    p[rangeLaw] = 0.0f; // (round 165x: Hush's own link and ranges, as projects saved before keep them)

    auto tone = [&] (double hz, double amp) {
        std::vector<float> s ((std::size_t) n);
        for (int i = 0; i < n; ++i) s[(std::size_t) i] = (float) (amp * std::sin (2.0 * M_PI * hz * i / sr));
        return s;
    };
    auto amplitude = [&] (const std::vector<float>& x, double hz) {
        double re = 0.0, im = 0.0, ws = 0.0;
        for (int i = n / 2; i < n; ++i)
        {
            const auto w = 0.5 - 0.5 * std::cos (2.0 * M_PI * (i - n / 2) / (n / 2));
            re += w * x[(std::size_t) i] * std::cos (2.0 * M_PI * hz * i / sr);
            im += w * x[(std::size_t) i] * std::sin (2.0 * M_PI * hz * i / sr);
            ws += w;
        }
        return 2.0 * std::sqrt (re * re + im * im) / ws;
    };
    auto runLR = [&] (const lynx::EffectParams& q, std::vector<float> l, std::vector<float> r, const std::vector<float>* sc = nullptr) {
        lynx::EffectSlot slot = lynx::makeEffectSlot (EffectKind::hushDeEsser);
        slot.params = q;
        auto fx = lynx::makeEffect (slot);
        fx->prepare (sr);
        for (int i = 0; i < n; i += 512)
        {
            if (sc != nullptr) fx->setSidechain (sc->data() + i, sc->data() + i);
            fx->process (l.data() + i, r.data() + i, std::min (512, n - i), q, 120.0);
        }
        return std::make_pair (l, r);
    };
    // the mix: 300 Hz + sibilance at 7 kHz + air at 18 kHz
    auto mix = tone (300.0, 0.3);
    const auto ess = tone (7000.0, 0.3), air = tone (18000.0, 0.05);
    for (int i = 0; i < n; ++i) mix[(std::size_t) i] += ess[(std::size_t) i] + air[(std::size_t) i];

    // low-pass handle at 12 kHz: 7 kHz is still turned down, 18 kHz passes untouched
    {
        const auto [l0, r0] = runLR (p, mix, mix);
        auto q = p;
        q[lowPass] = (float) (std::log (12000.0 / 2000.0) / std::log (10.0));
        const auto [l1, r1] = runLR (q, mix, mix);
        std::printf ("  hush low-pass 12 kHz: 7 kHz %.4f -> %.4f, 18 kHz %.4f -> %.4f\n", amplitude (l0, 7000.0), amplitude (l1, 7000.0), amplitude (l0, 18000.0), amplitude (l1, 18000.0));
        CHECK (amplitude (l1, 7000.0) < 0.3 * 0.45);
        CHECK (amplitude (l1, 18000.0) > 0.04); // (the Linkwitz-Riley band edge takes a little of it while the de-esser works)
        CHECK (amplitude (l0, 18000.0) < 0.05 * 0.6);
    }
    // stereo link 0: sibilance only on the left leaves the right alone
    {
        auto q = p;
        q[stereoLink] = 0.0f;
        const auto right = tone (300.0, 0.3);
        auto left = right;
        for (int i = 0; i < n; ++i) left[(std::size_t) i] += ess[(std::size_t) i];
        const auto [l1, r1] = runLR (q, left, right);
        const auto [l2, r2] = runLR (p, left, right);
        std::printf ("  hush link 0: right 300 Hz %.4f (linked %.4f)\n", amplitude (r1, 300.0), amplitude (r2, 300.0));
        CHECK (amplitude (l1, 7000.0) < 0.3 * 0.45);
        CHECK (std::abs (amplitude (r1, 300.0) - 0.3) < 0.01);
    }
    // MID: sibilance only in the side (opposite polarity) is not detected
    {
        auto q = p;
        q[linkMid] = 1.0f;
        q[mode] = 1.0f; // wide band, so the whole signal shows it
        auto left = tone (300.0, 0.3), right = left;
        for (int i = 0; i < n; ++i) { left[(std::size_t) i] += ess[(std::size_t) i]; right[(std::size_t) i] -= ess[(std::size_t) i]; }
        const auto [l1, r1] = runLR (q, left, right);
        auto q2 = p;
        q2[mode] = 1.0f;
        const auto [l2, r2] = runLR (q2, left, right);
        CHECK (std::abs (amplitude (l1, 300.0) - 0.3) < 0.01);
        CHECK (amplitude (l2, 300.0) < 0.3 * 0.5);
    }
    // external side chain: a dull signal is turned down when the side chain hisses
    {
        auto q = p;
        q[external] = 1.0f;
        q[mode] = 1.0f;
        const auto dull = tone (300.0, 0.3);
        const auto [l1, r1] = runLR (q, dull, dull, &ess);
        const auto [l2, r2] = runLR (p, dull, dull);
        CHECK (amplitude (l1, 300.0) < 0.3 * 0.5);
        CHECK (std::abs (amplitude (l2, 300.0) - 0.3) < 0.01);
    }
    // allround: a loud full-band signal makes the same sibilance trigger less
    {
        auto loud = tone (300.0, 0.9);
        for (int i = 0; i < n; ++i) loud[(std::size_t) i] += (float) (0.1 * ess[(std::size_t) i]);
        auto q = p;
        q[allround] = 1.0f;
        const auto [l1, r1] = runLR (q, loud, loud);
        const auto [l2, r2] = runLR (p, loud, loud);
        std::printf ("  hush allround: 7 kHz %.4f (single vocal %.4f)\n", amplitude (l1, 7000.0), amplitude (l2, 7000.0));
        CHECK (amplitude (l1, 7000.0) > amplitude (l2, 7000.0) * 1.2);
    }
    // look-ahead time = latency, known when the slot's effect is built; input level
    {
        auto q = p;
        q[lookaheadTime] = lookaheadTimeValue (10.0);
        lynx::EffectSlot slot = lynx::makeEffectSlot (EffectKind::hushDeEsser);
        slot.params = q;
        auto fx = lynx::makeEffect (slot);
        fx->prepare (sr);
        CHECK (fx->latencySamples() == 441);
        q[input] = 0.75f; // +6 dB
        const auto [l1, r1] = runLR (q, tone (300.0, 0.1), tone (300.0, 0.1));
        CHECK (std::abs (amplitude (l1, 300.0) - 0.1 * std::pow (10.0, 6.0 / 20.0)) < 0.003);
    }
    // saved projects: the slot's parameters are positional, older projects (9 values) keep the new ones at defaults
    {
        auto slot = lynx::makeEffectSlot (EffectKind::hushDeEsser);
        CHECK (slot.params[lowPass] == 1.0f && slot.params[display] == 1.0f);
        CHECK (lynx::formatEffectParam (EffectKind::hushDeEsser, lowPass, 1.0f, 120.0) == "off");
        CHECK (lynx::formatEffectParam (EffectKind::hushDeEsser, allround, 1.0f, 120.0) == "allround");
    }
}
