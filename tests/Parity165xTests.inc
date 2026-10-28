// --- Round 165x (parity165x): Glow ← CLA-2A attack and AUTO MAKEUP's reference, Hush ← Pro-DS ranges and stereo link,
// maxEffectParams 192, Ember Classic's footer line (docs/analise/REA_rodada165x.md) --------------------------------------

#include "core/ParamHelp.h"
#include "core/EffectPresets.h"

namespace p165x
{
// Glow's classic voicing on a 1 kHz tone: 1 s at −40 dBFS, then `burstDb`; the gain (dB, against the probe before the
// burst) in 1 ms windows from the burst's start
inline std::vector<double> glowOnset (const lynx::EffectParams& p, double burstDb, int windows)
{
    using namespace lynx;
    constexpr double sr = 48000.0;
    const std::size_t pre = 48000, n = pre + 48 * (std::size_t) windows + 4800;
    std::vector<float> l (n), r;
    for (std::size_t i = 0; i < n; ++i)
        l[i] = (float) (std::pow (10.0, (i >= pre ? burstDb : -40.0) / 20.0) * std::sin (2.0 * 3.141592653589793 * 1000.0 * (double) i / sr));
    const auto x = l;
    r = l;
    auto slot = makeEffectSlot (EffectKind::glowLeveler);
    slot.params = p;
    auto fx = makeEffect (slot);
    fx->prepare (sr);
    fx->reset();
    for (std::size_t off = 0; off < n; off += 256)
        fx->process (l.data() + off, r.data() + off, (int) std::min<std::size_t> (256, n - off), p, 120.0);
    const auto gainAt = [&] (std::size_t from, std::size_t len) {
        double ey = 0.0, ex = 0.0;
        for (auto i = from; i < from + len; ++i) { ey += (double) l[i] * l[i]; ex += (double) x[i] * x[i]; }
        return 10.0 * std::log10 (ey / ex);
    };
    const auto base = gainAt (pre - 9600, 4800);
    std::vector<double> out;
    for (int k = 0; k < windows; ++k)
        out.push_back (gainAt (pre + 48 * (std::size_t) k, 48) - base);
    return out;
}
} // namespace p165x

TEST (glow_classic_measured_attack_round165x)
{
    using namespace lynx;
    namespace g = mixsuite::glow;
    auto p = makeEffectSlot (EffectKind::glowLeveler).params;
    p[g::mode] = 1.0f; // Limit
    p[g::peakReduction] = 0.80f;
    // [EXEC] the trial (fresh instance, Limit 80, 0 dBFS): −0.90 / −2.93 / −4.77 / −7.86 / −13.06 / −21.66 dB in the 1 ms
    // windows starting at 0 / 1 / 2 / 4 / 10 / 49 ms — the transient goes through, the gain falls ≈ 2 dB per ms
    static constexpr int at[] = { 0, 1, 2, 4, 10, 49 };
    static constexpr double trial[] = { -0.90, -2.93, -4.77, -7.86, -13.06, -21.66 };
    auto got = p165x::glowOnset (p, 0.0, 50);
    for (int k = 0; k < 6; ++k)
    {
        if (std::abs (got[(std::size_t) at[k]] - trial[k]) >= 3.5)
            std::printf ("  glow limit 80 onset: %d ms %.2f dB (trial %.2f)\n", at[k], got[(std::size_t) at[k]], trial[k]);
        CHECK (std::abs (got[(std::size_t) at[k]] - trial[k]) < 3.5);
    }
    CHECK (got[0] > -2.0); // (round 123's attack: −13.5 dB in the first millisecond)
    // Compress 62.6, −12 dBFS: −0.79 / −2.43 / −3.87 / −6.16 / −9.49 / −11.31
    p[g::mode] = 0.0f;
    p[g::peakReduction] = 0.626f;
    static constexpr double trialC[] = { -0.79, -2.43, -3.87, -6.16, -9.49, -11.31 };
    got = p165x::glowOnset (p, -12.0, 50);
    for (int k = 0; k < 6; ++k)
    {
        if (std::abs (got[(std::size_t) at[k]] - trialC[k]) >= 1.6)
            std::printf ("  glow compress 62.6 onset: %d ms %.2f dB (trial %.2f)\n", at[k], got[(std::size_t) at[k]], trialC[k]);
        CHECK (std::abs (got[(std::size_t) at[k]] - trialC[k]) < 1.6);
    }
    // round 123's attack stays for projects saved before (Opto release Old): Limit 80 clamps within the first millisecond
    p[g::mode] = 1.0f;
    p[g::peakReduction] = 0.80f;
    p[g::releaseLaw] = 0.0f;
    got = p165x::glowOnset (p, 0.0, 5);
    CHECK (got[0] < -8.0);
}

TEST (glow_auto_makeup_reference_round165x)
{
    using namespace lynx;
    namespace g = mixsuite::glow;
    namespace o = vmatch::opto;
    const auto& info = effectInfo (EffectKind::glowLeveler);
    CHECK ((int) info.params.size() == g::count && g::makeupRef == 11 && g::vuDisplay == g::count - 1);
    auto p = makeEffectSlot (EffectKind::glowLeveler).params;
    CHECK (p[g::makeupRef] == 0.46f); // round 152's reference: older projects unchanged
    // the level of a −40 dBFS tone (below the threshold)
    const auto probe = [] (const EffectParams& q) {
        auto slot = makeEffectSlot (EffectKind::glowLeveler);
        slot.params = q;
        auto fx = makeEffect (slot);
        fx->prepare (48000.0);
        std::vector<float> l (48000), r;
        for (std::size_t i = 0; i < l.size(); ++i)
            l[i] = 0.01f * (float) std::sin (2.0 * 3.141592653589793 * 1000.0 * (double) i / 48000.0);
        r = l;
        const auto x = l;
        for (std::size_t off = 0; off < l.size(); off += 256)
            fx->process (l.data() + off, r.data() + off, (int) std::min<std::size_t> (256, l.size() - off), q, 120.0);
        double ey = 0, ex = 0;
        for (std::size_t i = 24000; i < l.size(); ++i) { ey += (double) l[i] * l[i]; ex += (double) x[i] * x[i]; }
        return 10.0 * std::log10 (ey / ex);
    };
    p[g::peakReduction] = 0.80f;
    const auto off = probe (p);
    p[g::autoMakeup] = 1.0f;
    // [EXEC] switched on at 46, the dial moved to 80: +11.37 dB (round 152's law, kept for its projects)
    CHECK (std::abs (probe (p) - off - 11.37) < 0.1);
    // switched on at 80: no make-up there (the trial: 0.00 dB at every level)
    p[g::makeupRef] = 0.80f;
    CHECK (std::abs (probe (p) - off) < 0.05);
    // on at 30, the dial to 80: +12.39 dB in the trial
    p[g::makeupRef] = 0.30f;
    CHECK (std::abs (probe (p) - off - 12.39) < 0.1);
    // the editor's link: switching on takes the dial as the reference ...
    auto q = makeEffectSlot (EffectKind::glowLeveler).params;
    q[g::peakReduction] = 0.62f;
    auto edits = g::linkedEdit (q, g::autoMakeup, 1.0f);
    CHECK (edits.size() == 2 && edits[1].first == g::makeupRef && edits[1].second == 0.62f);
    // ... switching off leaves the make-up in the Gain dial (the trial keeps it: its Gain knob moved)
    q[g::autoMakeup] = 1.0f;
    q[g::makeupRef] = 0.46f;
    q[g::peakReduction] = 0.80f;
    edits = g::linkedEdit (q, g::autoMakeup, 0.0f);
    CHECK (edits.size() == 2 && edits[1].first == g::gain);
    if (edits.size() == 2)
    {
        // [EXEC] the trial's saved Gain after on at 46 → 80: 53.79 (from 40)
        CHECK (std::abs (edits[1].second * 100.0 - 53.79) < 0.1);
        CHECK (std::abs (o::makeupDb (edits[1].second * 100.0) - o::makeupDb (40.0) - 11.37) < 0.02);
    }
    CHECK (g::linkedEdit (q, g::gain, 0.5f).size() == 1);
    // a CLA-2A state saved that way (Gain 53.79, Peak reduction 80, Auto Makeup on): the make-up is in the Gain already
    int n = 0;
    std::string values;
    for (int i = 0; i < 208; ++i)
        values += i == 38 ? "53.79 " : i == 74 ? "80 " : i == 153 ? "50 " : i == 204 ? "100 " : i == 140 ? "2 " : i == 207 ? "1 " : "0 ";
    const auto s = vm123::wavesSlot ("CLA-2A", values.c_str(), n);
    CHECK (s.kind == EffectKind::glowLeveler && s.params[g::autoMakeup] == 1.0f && std::abs (s.params[g::makeupRef] - 0.80f) < 1e-6);
    // the trial's level at −40 dBFS for that state: 8.34 (Compress 80, Gain 40) + 11.37
    if (std::abs (probe (s.params) - (off + 11.37)) >= 0.15)
        std::printf ("  glow translated auto makeup: %.2f dB (expected %.2f)\n", probe (s.params), off + 11.37);
    CHECK (std::abs (probe (s.params) - (off + 11.37)) < 0.15);
    // [EXEC] the VU DISPLAY knob (RealWorld 17: 0 In, 1 GR, 2 Out) is translated; a new Glow shows the gain reduction
    CHECK (s.params[g::vuDisplay] == 0.5f && g::vuModeOf (s.params[g::vuDisplay]) == 1);
    CHECK (makeEffectSlot (EffectKind::glowLeveler).params[g::vuDisplay] == 0.0f && g::vuModeOf (0.0f) == 0 && g::vuModeOf (1.0f) == 2);
    values.clear();
    for (int i = 0; i < 208; ++i)
        values += i == 38 ? "40 " : i == 74 ? "46 " : i == 17 ? "2 " : "0 ";
    CHECK (vm123::wavesSlot ("CLA-2A", values.c_str(), n).params[g::vuDisplay] == 1.0f);
    CHECK (formatEffectParam (EffectKind::glowLeveler, g::vuDisplay, 1.0f, 120.0, makeEffectSlot (EffectKind::glowLeveler).params) == "output");
}

TEST (hush_extended_ranges_round165x)
{
    using namespace lynx;
    namespace h = mixsuite::hush;
    const auto& info = effectInfo (EffectKind::hushDeEsser);
    CHECK ((int) info.params.size() == h::count && h::linkSide == h::count - 1);
    auto p = makeEffectSlot (EffectKind::hushDeEsser).params;
    CHECK (p[h::rangeLaw] == 1.0f && h::extended (p));
    // [EXEC] the trial's Input / Output Level texts
    CHECK (std::abs (h::levelDb (0.5f, true)) < 1e-6 && std::abs (h::levelDb (1.0f, true) - 36.0) < 1e-4);
    CHECK (std::abs (h::levelDb (0.251f, true) + 17.93) < 0.01 && std::abs (h::levelDb (0.101f, true) + 33.47) < 0.02);
    CHECK (std::abs (h::levelDb (0.001f, true) + 113.64) < 0.02 && h::levelDb (0.0f, true) < -150.0);
    for (const double db : { -60.0, -21.6, -18.0, 0.0, 7.2, 36.0 })
        CHECK (std::abs (h::levelDb (h::levelValue (db, true), true) - db) < 0.01);
    // the high-pass to 20 kHz: 0.505 → 6397.8 Hz, 1 → 20 kHz; the default stays at 6 kHz
    CHECK (std::abs (h::hpHz (0.505f, true) - 6397.8) < 1.0 && std::abs (h::hpHz (1.0f, true) - 20000.0) < 0.5);
    CHECK (std::abs (h::hpHz (p[h::frequency], true) - h::hz (0.53f)) < 10.0);
    CHECK (formatEffectParam (EffectKind::hushDeEsser, h::output, 1.0f, 120.0, p) == "+36.0 dB");
    CHECK (formatEffectParam (EffectKind::hushDeEsser, h::output, 0.0f, 120.0, p) == "-inf dB");
    CHECK (formatEffectParam (EffectKind::hushDeEsser, h::frequency, 1.0f, 120.0, p) == "20.00 kHz");
    // a project saved before keeps the old law (±12 dB, 2 … 16 kHz), its texts too
    auto slot = makeEffectSlot (EffectKind::hushDeEsser);
    upgradeLoadedEffectParams (slot, (std::size_t) h::rangeLaw);
    CHECK (slot.params[h::rangeLaw] == 0.0f && ! h::extended (slot.params));
    CHECK (formatEffectParam (EffectKind::hushDeEsser, h::output, 1.0f, 120.0, slot.params) == "+12.0 dB");
    slot = makeEffectSlot (EffectKind::hushDeEsser);
    upgradeLoadedEffectParams (slot, (std::size_t) h::count);
    CHECK (slot.params[h::rangeLaw] == 1.0f);
    // the classic voicing keeps its own ranges
    auto c = makeEffectSlot (EffectKind::hushDeEsser).params;
    c[h::voicing] = 1.0f;
    CHECK (! h::extended (c) && formatEffectParam (EffectKind::hushDeEsser, h::output, 1.0f, 120.0, c) == "+12.0 dB");
    // the output level: +18 dB on the extended law, +6 dB on the old (0.75)
    const auto gain = [] (const EffectParams& q) {
        auto fx = makeEffect (EffectKind::hushDeEsser);
        fx->prepare (48000.0);
        std::vector<float> l (9600), r;
        for (std::size_t i = 0; i < l.size(); ++i)
            l[i] = 0.01f * (float) std::sin (2.0 * 3.141592653589793 * 1000.0 * (double) i / 48000.0);
        r = l;
        const auto x = l;
        for (std::size_t off = 0; off < l.size(); off += 256)
            fx->process (l.data() + off, r.data() + off, (int) std::min<std::size_t> (256, l.size() - off), q, 120.0);
        double ey = 0, ex = 0;
        for (std::size_t i = 4800; i < 9000; ++i) { ey += (double) l[i] * l[i]; ex += (double) x[i] * x[i]; }
        return 10.0 * std::log10 (ey / ex);
    };
    auto q = p;
    q[h::threshold] = 1.0f;
    q[h::output] = 0.75f;
    CHECK (std::abs (gain (q) - 18.0) < 0.05);
    q[h::rangeLaw] = 0.0f;
    CHECK (std::abs (gain (q) - 6.0) < 0.05);
}

namespace p165x
{
// the vocal voicing on L / R test material (sibilance 9 kHz, 1 kHz / 1.5 kHz probes in L / R, −40 dB each): the
// steady gains of the L probe into L and R and of the R probe into R (lock-in, last 0.5 s of 2 s)
struct LinkGains { double ll, lr, rr; };
inline LinkGains hushLink (const lynx::EffectParams& q, double sibL, double sibR)
{
    using namespace lynx;
    constexpr double sr = 48000.0, pi = 3.141592653589793;
    const std::size_t n = 96000;
    std::vector<float> l (n), r (n);
    for (std::size_t i = 0; i < n; ++i)
    {
        const double t = (double) i / sr, s = std::sin (2.0 * pi * 9000.0 * t);
        l[i] = (float) (sibL * s + 0.01 * std::sin (2.0 * pi * 1000.0 * t));
        r[i] = (float) (sibR * s + 0.01 * std::sin (2.0 * pi * 1500.0 * t));
    }
    auto fx = makeEffect (EffectKind::hushDeEsser);
    fx->prepare (sr);
    for (std::size_t off = 0; off < n; off += 256)
        fx->process (l.data() + off, r.data() + off, (int) std::min<std::size_t> (256, n - off), q, 120.0);
    const int lat = fx->latencySamples();
    const auto lock = [&] (const std::vector<float>& y, double f) {
        double re = 0.0;
        for (std::size_t i = 72000; i < n; ++i)
            re += (double) y[i] * std::sin (2.0 * pi * f * ((double) i - lat) / sr);
        return 2.0 * re / (double) (n - 72000) / 0.01;
    };
    return { lock (l, 1000.0), lock (r, 1000.0), lock (r, 1500.0) };
}
} // namespace p165x

TEST (hush_pro_ds_stereo_link_round165x)
{
    using namespace lynx;
    namespace h = mixsuite::hush;
    auto p = makeEffectSlot (EffectKind::hushDeEsser).params;
    p[h::vocalVoicing] = 1.0f; // Pro-DS's Single Vocal
    p[h::threshold] = 0.333f;  // −40 dB
    p[h::range] = 0.5f;        // 12 dB
    p[h::mode] = 1.0f;         // wide band
    // [EXEC] the trial, Stereo Link 0 %: the sibilant channel alone is turned down
    p[h::stereoLink] = 0.0f;
    auto g = p165x::hushLink (p, 0.3, 0.0);
    CHECK (g.ll < 0.7 && std::abs (g.rr - 1.0) < 0.02 && std::abs (g.lr) < 0.01);
    // 100 %: the mid signal — sibilance in anti-phase (side only) is not touched, the same gain on both channels
    p[h::stereoLink] = 1.0f;
    g = p165x::hushLink (p, 0.3, -0.3);
    CHECK (std::abs (g.ll - 1.0) < 0.02 && std::abs (g.rr - 1.0) < 0.02);
    g = p165x::hushLink (p, 0.3, 0.0);
    CHECK (g.ll < 0.8 && std::abs (g.ll - g.rr) < 0.02 && std::abs (g.lr) < 0.01);
    // 200 % Mid-only: the side passes — the L probe reaches R with (gain − 1) / 2, L keeps (gain + 1) / 2
    const auto g100 = g.ll;
    p[h::linkOnly] = 1.0f;
    g = p165x::hushLink (p, 0.3, 0.0);
    CHECK (std::abs (g.ll - (g100 + 1.0) / 2.0) < 0.03 && std::abs (g.lr - (g100 - 1.0) / 2.0) < 0.03);
    // 200 % Side-only: mid sibilance passes untouched; side sibilance is turned down on the side only
    p[h::linkSide] = 1.0f;
    g = p165x::hushLink (p, 0.3, 0.3);
    CHECK (std::abs (g.ll - 1.0) < 0.02 && std::abs (g.lr) < 0.01);
    g = p165x::hushLink (p, 0.3, -0.3);
    CHECK (g.ll < 0.95 && g.lr > 0.02);
    // the old law ignores Link only (Hush's own: 100 % = the mid signal of her states, as before)
    p[h::rangeLaw] = 0.0f;
    g = p165x::hushLink (p, 0.3, 0.0);
    CHECK (std::abs (g.ll - g100) < 0.03 && std::abs (g.lr) < 0.01);
}

TEST (hush_pro_ds_translation_ranges_round165x)
{
    using namespace lynx;
    namespace h = mixsuite::hush;
    // her first state's values with: high-pass 20 kHz (1), Stereo Link 150 % Side (0.75 / 1), input +7.2 dB (0.2),
    // output −∞ (−1)
    std::vector<float> st { 0.0f, 0.2715f, 6.0f, 0.0f, 0.75f, 1.0f, 12.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f,
                            0.0f, 0.0f, 0.0f, 0.2f, 0.0f, -1.0f, 0.0f, 0.0f };
    int n = 0;
    const auto s = m124x::substitute ("FabFilter Pro-DS", p165w::fabf (st), n);
    CHECK (s.kind == EffectKind::hushDeEsser && s.params[h::rangeLaw] == 1.0f);
    CHECK (std::abs (h::hpHz (s.params[h::frequency], true) - 20000.0) < 1.0);
    CHECK (std::abs (h::levelDb (s.params[h::input], true) - 7.2) < 0.01 && s.params[h::output] == 0.0f);
    CHECK (s.params[h::stereoLink] == 1.0f && std::abs (s.params[h::linkOnly] - 0.5f) < 1e-6 && s.params[h::linkSide] == 1.0f);
    // 50 % L/R
    st[4] = 0.25f;
    st[5] = 0.0f;
    const auto s2 = m124x::substitute ("FabFilter Pro-DS", p165w::fabf (st), n);
    CHECK (std::abs (s2.params[h::stereoLink] - 0.5f) < 1e-6 && s2.params[h::linkOnly] == 0.0f && s2.params[h::linkSide] == 0.0f);
}

TEST (max_effect_params_192_round165x)
{
    using namespace lynx;
    CHECK (maxEffectParams == 192);
    // every engine fits; Ember (160) can grow again
    for (int k = 0; k <= (int) lastEffectKind; ++k)
        CHECK (effectInfo ((EffectKind) k).params.size() <= (std::size_t) maxEffectParams);
    CHECK ((int) effectInfo (EffectKind::ember).params.size() == 160);
    // an engine with more than 160 parameters (the Control Surface: one per output) round-trips through a saved file
    // (a user preset: every parameter by name, like a project's list; the project reader itself is checked by the
    // app's --selftest "old project (160 of n params)" cases)
    const auto roundTrip = [] (const EffectSlot& fx, int keep) {
        auto text = presets::toText (fx.kind, "t", fx.params);
        if (keep >= 0) // a file saved before (fewer parameters): keep the header and the first `keep` lines
        {
            std::size_t at = 0;
            for (int line = 0; line < 3 + keep && at != std::string::npos; ++line)
                at = text.find ('\n', at) + 1;
            text = text.substr (0, at);
        }
        MixerTrack t;
        if (auto loaded = presets::fromText (fx.kind, text))
        {
            auto back = makeEffectSlot (fx.kind);
            back.params = loaded->params;
            t.effects.push_back (back);
        }
        return t;
    };
    auto slot = makeEffectSlot (EffectKind::controlSurface);
    CHECK ((int) effectInfo (EffectKind::controlSurface).params.size() == maxEffectParams);
    for (int i = 0; i < maxEffectParams; ++i)
        slot.params[(std::size_t) i] = (float) ((i * 37) % 100) / 100.0f;
    auto back = roundTrip (slot, -1);
    CHECK (back.effects.size() == 1);
    if (back.effects.size() == 1)
    {
        bool same = true;
        for (int i = 0; i < maxEffectParams; ++i)
            same = same && std::abs (back.effects[0].params[(std::size_t) i] - slot.params[(std::size_t) i]) < 1e-4f;
        CHECK (same);
    }
    // a file saved with 160 parameters (before this round): those load as saved, the rest at their defaults
    const auto fresh = makeEffectSlot (EffectKind::controlSurface);
    back = roundTrip (slot, 160);
    CHECK (back.effects.size() == 1);
    if (back.effects.size() == 1)
    {
        CHECK (std::abs (back.effects[0].params[159] - slot.params[159]) < 1e-4f);
        CHECK (back.effects[0].params[170] == fresh.params[170] && back.effects[0].params[191] == fresh.params[191]);
    }
    // an Ember saved with its 160 parameters loads unchanged
    auto ember = makeEffectSlot (EffectKind::ember);
    for (int i = 0; i < 160; ++i)
        ember.params[(std::size_t) i] = (float) ((i * 13) % 97) / 97.0f;
    back = roundTrip (ember, -1);
    CHECK (back.effects.size() == 1);
    if (back.effects.size() == 1)
    {
        bool same = true;
        for (int i = 0; i < 160; ++i)
            same = same && std::abs (back.effects[0].params[(std::size_t) i] - ember.params[(std::size_t) i]) < 1e-4f;
        CHECK (same);
    }
}

TEST (ember_classic_footer_line_round165x)
{
    using namespace lynx;
    namespace e = mixsuite::ember;
    auto p = makeEffectSlot (EffectKind::ember).params;
    auto s = paramhelp::summary (EffectKind::ember, p);
    CHECK (s.param == (int) e::style);
    p[e::voicing] = 1.0f;
    p[e::classicStyle] = 0.0f; // A
    s = paramhelp::summary (EffectKind::ember, p);
    CHECK (s.param == (int) e::classicStyle && s.text != nullptr && std::string (s.text).find ("tape preamp") != std::string::npos);
    p[e::classicStyle] = 1.0f; // P
    s = paramhelp::summary (EffectKind::ember, p);
    CHECK (s.text != nullptr && std::string (s.text).find ("pentode") != std::string::npos);
    CHECK (std::string (s.text).find ("tube") == std::string::npos);
}
