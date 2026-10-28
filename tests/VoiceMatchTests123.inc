// Round 123: matched voicings — LYNX's substitutes against golden curves measured on the official trials at her
// settings (docs/analise/REA_rodada123.md). The golden numbers are the real plug-ins' responses (black-box renders).


#include "core/VoiceMatch.h"
namespace vm123
{
constexpr double sr = 48000.0;

inline lynx::EffectSlot wavesSlot (const char* waves, const char* values, int& translated)
{
    using namespace lynx;
    PluginIdentity id;
    id.format = ForeignPlugin::Format::vst3;
    id.name = "WaveShell1-VST3 16.0_x64";
    id.state = substtest::wavesComponent (waves, values);
    const auto* e = findPluginSubstitute (id, false);
    CHECK (e != nullptr);
    return e != nullptr ? makeSubstituteSlot (*e, &id, &translated) : EffectSlot {};
}

// steady gain (dB) of an effect for a 1 kHz sine at `db` dBFS peak (last 150 ms of 0.6 s)
inline double sineGainDb (const lynx::EffectSlot& slot, double hz, double db)
{
    const auto n = (std::size_t) (0.6 * sr);
    std::vector<float> l (n), r (n);
    for (std::size_t i = 0; i < n; ++i)
        l[i] = r[i] = (float) (std::pow (10.0, db / 20.0) * std::sin (2.0 * 3.141592653589793 * hz * (double) i / sr));
    const auto x = l;
    auto fx = lynx::makeEffect (slot);
    fx->prepare (sr);
    fx->reset();
    for (std::size_t off = 0; off < n; off += 256)
        fx->process (l.data() + off, r.data() + off, (int) std::min<std::size_t> (256, n - off), slot.params, 120.0);
    double ey = 0.0, ex = 0.0;
    for (auto i = n - (std::size_t) (0.15 * sr); i < n; ++i)
    {
        ey += (double) l[i] * l[i];
        ex += (double) x[i] * x[i];
    }
    return 10.0 * std::log10 (ey / ex);
}

constexpr const char* herSsl = "-1.3 1.8 1 0.35 * -30 0 0 0 0.2 * 0 0 0 -6 60 0 0.5 2.5 3 3.5 1.5 0 4.8 10.16 0 0 116 30 1 0 0 * * 1 * * 7 -18 0 0 * * * * * * -18 -18 -18 -18 -18 -18 -18 -18 0 0";
} // namespace vm123

TEST (ssl_e_channel_matched_voicing_round123)
{
    using namespace lynx;
    namespace r = mixsuite::ridge;
    int n = 0;
    auto s = vm123::wavesSlot ("SSLChannel", vm123::herSsl, n);
    CHECK (n == 32 && s.kind == EffectKind::ridgeStrip);
    CHECK (s.params[r::voicing] == 1.0f && s.params[r::filterSplit] == 1.0f && s.params[r::order] == 1.0f);
    CHECK (std::abs (r::hpfHz (s.params[r::hpf]) - 116.0) < 0.01 && std::abs (r::eQ (s.params[r::hmfQ]) - 1.5) < 1e-3);
    CHECK (std::abs (r::eCompThresholdDb (s.params[r::compThreshold]) + 1.3) < 1e-3 && std::abs (r::ratio (s.params[r::compRatio]) - 1.8) < 1e-3);
    CHECK (formatEffectParam (EffectKind::ridgeStrip, r::hmfQ, s.params[r::hmfQ], 120.0, s.params) == "Q 1.50");

    // the console's dial: Q 3.5 at the left .. 0.1 at the right, its own taper
    CHECK (std::abs (vmatch::echannel::qFromDial (0.5) - 1.5) < 1e-9 && std::abs (vmatch::echannel::dialFromQ (2.0) - 0.3) < 1e-9);

    // EQ + filters at her state vs the official plug-in's impulse response (dynamics bypassed), 48 kHz
    vmatch::echannel::Settings e;
    e.hpHz = 116; e.lfGain = -6; e.lfHz = 60; e.lmfGain = 0; e.lmfKHz = 0.5; e.lmfQ = 2.5;
    e.hmfGain = 3; e.hmfKHz = 3.5; e.hmfQ = 1.5; e.hfGain = 4.8; e.hfKHz = 10.16;
    static constexpr double golden[][2] = { { 40, -28.667 }, { 60, -17.681 }, { 100, -6.385 }, { 150, -2.861 }, { 300, -0.928 },
                                            { 1000, 0.311 }, { 2000, 1.481 }, { 3500, 3.960 }, { 5000, 5.172 }, { 8000, 4.828 },
                                            { 10160, 4.693 }, { 14000, 4.387 }, { 18000, 3.391 } };
    double worst = 0.0;
    for (const auto& g : golden)
        worst = std::max (worst, std::abs (vmatch::echannel::responseDb (e, vm123::sr, g[0]) - g[1]));
    CHECK (worst < 0.08); // (0.07 dB at 40 Hz, 28 dB down the high-pass skirt; 0.02 dB elsewhere)

    // the compressor at her state (EQ off): 1 kHz sine, steady gain vs the official plug-in's stairs (its own
    // make-up +0.88 dB below the threshold, about 1.4:1 above)
    s.params[r::eqOn] = 0.0f;
    s.params[r::hpf] = 0.0f;
    static constexpr double comp[][2] = { { -50, 0.879 }, { -26, 0.879 }, { -20, 0.610 }, { -14, -0.331 }, { -8, -1.702 }, { -2, -3.369 } };
    worst = 0.0;
    for (const auto& c : comp)
        worst = std::max (worst, std::abs (vm123::sineGainDb (s, 1000.0, c[0]) - c[1]));
    CHECK (worst < 0.15); // (0.11 dB in the knee, isolated tones vs her stairs)

    // voicing 0 keeps Ridge's own engine (older projects): the same settings sound different there
    auto classic = s;
    classic.params[r::voicing] = 0.0f;
    CHECK (std::abs (vm123::sineGainDb (classic, 1000.0, -50.0) - 0.879) > 0.5);
}

TEST (deesser_matched_voicing_round123)
{
    using namespace lynx;
    namespace h = mixsuite::hush;
    int n = 0;
    auto s = vm123::wavesSlot ("DeEsser", "5506 2 -24.2 7 0.5 10 * * * * 0 3.98 32 1 0", n);
    CHECK (n == 5 && s.kind == EffectKind::hushDeEsser && s.params[h::voicing] == 1.0f);
    CHECK (std::abs (h::hz (s.params[h::frequency]) - 5506.0) < 0.5 && std::abs (h::classicThresholdDb (s.params[h::threshold]) + 24.2) < 1e-3);
    CHECK (s.params[h::mode] == 0.0f && s.params[h::detector] == 0.0f && s.params[h::listen] == 0.0f);
    CHECK (formatEffectParam (EffectKind::hushDeEsser, h::threshold, s.params[h::threshold], 120.0, s.params) == "-24.2 dB");
    // 6 kHz sine: steady gain vs the official plug-in's stairs at her state (no range control: it floors near -11 dB);
    // 1 kHz passes untouched
    static constexpr double golden[][2] = { { -30, 0.0 }, { -22, 0.0 }, { -18, -1.414 }, { -14, -4.310 }, { -10, -6.775 }, { -6, -8.630 }, { -2, -9.832 } };
    double worst = 0.0;
    for (const auto& g : golden)
        worst = std::max (worst, std::abs (vm123::sineGainDb (s, 6000.0, g[0]) - g[1]));
    CHECK (worst < 0.2);
    CHECK (std::abs (vm123::sineGainDb (s, 1000.0, -2.0)) < 0.05);
}

TEST (opto_and_program_compressor_voicings_round123)
{
    using namespace lynx;
    // her CLA-2A: gain 48.96, peak reduction 62.6, compress, Hi Freq 50 -> Glow Leveler "opto classic", dials 1:1
    std::string values;
    for (int i = 0; i < 208; ++i)
        values += i == 38 ? "48.96 " : i == 74 ? "62.6 " : i == 153 ? "50 " : i == 204 ? "100 " : i == 140 ? "2 " : "0 "; // round 152: + Mix 100 %, Analog off (her states)
    int n = 0;
    auto s = vm123::wavesSlot ("CLA-2A", values.c_str(), n);
    namespace g = mixsuite::glow;
    CHECK (n == 8 && s.kind == EffectKind::glowLeveler && s.params[g::voicing] == 1.0f);
    CHECK (std::abs (s.params[g::peakReduction] - 0.626f) < 1e-5f && std::abs (s.params[g::gain] - 0.4896f) < 1e-5f);
    // 1 kHz steady gain vs the official plug-in's stairs: +16.0 dB below the threshold, -1.2 dB at -6 dBFS (her state)
    CHECK (std::abs (vm123::sineGainDb (s, 1000.0, -50.0) - 15.98) < 0.1);
    CHECK (std::abs (vm123::sineGainDb (s, 1000.0, -6.0) - (-1.2)) < 0.6);
    // the gain dial's measured law (level below the threshold) and the peak reduction / Hi Freq sensitivity shifts
    CHECK (std::abs (vmatch::opto::makeupDb (80.0) - 33.68) < 1e-9 && std::abs (vmatch::opto::sensitivityDb (62.6, 50.0)) < 1e-9);
    CHECK (std::abs (vmatch::opto::sensitivityDb (80.0, 0.0) - (-34.05 + 24.78 + 6.15)) < 1e-9);

    // her Renaissance Compressor -> Compressor "vintage program": -15 dB, 2.96:1, 0.5 ms, 160 ms, gain 0
    n = 0;
    auto c = vm123::wavesSlot ("RComp", "160.31 -15 0 * * * * * 0.5 2.96 56.23 10 0 0 149.62 120 0 0 1 0.4 0 1.5 0 * * * 100 0", n);
    CHECK (n == 6 && c.kind == EffectKind::compressor && c.params[8] == 1.0f && c.params[6] == 1.0f);
    // 1 kHz steady gain vs the official plug-in's stairs at her state: 0 / -0.4 / -1.8 / -4.6 / -8.5 / -12.8 dB
    static constexpr double golden[][2] = { { -30, 0.0 }, { -24, -0.4 }, { -18, -1.8 }, { -12, -4.6 }, { -6, -8.5 }, { 0, -12.8 } };
    double worst = 0.0;
    for (const auto& gd : golden)
        worst = std::max (worst, std::abs (vm123::sineGainDb (c, 1000.0, gd[0]) - gd[1]));
    CHECK (worst < 0.35);
    auto fx = makeEffect (c);
    fx->prepare (48000.0);
    std::vector<float> l (256, 0.0f), r (256, 0.0f);
    fx->process (l.data(), r.data(), 256, c.params, 120.0);
    CHECK (fx->latencySamples() == vmatch::progcomp::lookahead);
}
