// Round 148 (vox148): LYNX Tune's held notes in noise — the PSOLA marks follow the waveform's repetition (period
// cross-correlation, sub-sample peak) instead of the single highest peak (docs/analise/REA_rodada148.md §1)

namespace vox148
{
// a held harmonic note (1/k, 12 harmonics) at `hz` plus white noise `snrDb` under it, mono, `sec` long
std::vector<float> noisyNote (double hz, double snrDb, double sec, double sr, std::uint32_t seed = 148)
{
    const int n = (int) (sec * sr);
    std::vector<float> x ((std::size_t) n);
    double s2 = 0.0;
    for (int i = 0; i < n; ++i)
    {
        double v = 0.0;
        for (int k = 1; k <= 12; ++k)
            v += std::sin (2.0 * 3.141592653589793 * k * hz * i / sr) / k;
        x[(std::size_t) i] = (float) (0.15 * v);
        s2 += 0.0225 * v * v;
    }
    const auto noise = std::sqrt (s2 / n) * std::pow (10.0, -snrDb / 20.0) * std::sqrt (3.0); // uniform: rms = a / sqrt 3
    auto state = seed;
    for (auto& v : x)
    {
        state = state * 1664525u + 1013904223u;
        v += (float) (noise * ((double) (state >> 8) / 8388608.0 - 1.0));
    }
    return x;
}

// rms deviation (cents) of the fundamental's frequency over 40 ms windows (phase advance of the DFT bin at `hz`
// between windows 10 ms apart), from `from` to `to` seconds: how much a held note wobbles
double wobbleCents (const std::vector<float>& y, double hz, double sr, double from, double to)
{
    const int w = (int) (0.04 * sr), hop = (int) (0.01 * sr);
    auto bin = [&] (int s) {
        double re = 0.0, im = 0.0;
        for (int i = 0; i < w; ++i)
        {
            const auto win = 0.5 - 0.5 * std::cos (2.0 * 3.141592653589793 * i / w);
            const auto a = 2.0 * 3.141592653589793 * hz * (s + i) / sr;
            re += win * y[(std::size_t) (s + i)] * std::cos (a);
            im -= win * y[(std::size_t) (s + i)] * std::sin (a);
        }
        return std::atan2 (im, re);
    };
    std::vector<double> c;
    for (int s = (int) (from * sr); s + hop + w < (int) (to * sr); s += hop)
    {
        auto d = bin (s + hop) - bin (s);
        while (d > 3.141592653589793) d -= 2.0 * 3.141592653589793;
        while (d < -3.141592653589793) d += 2.0 * 3.141592653589793;
        const auto f = hz + d / (2.0 * 3.141592653589793 * hop / sr);
        c.push_back (1200.0 * std::log2 (f / hz));
    }
    double m = 0.0, v = 0.0;
    for (auto x : c) m += x;
    m /= (double) c.size();
    for (auto x : c) v += (x - m) * (x - m);
    return std::sqrt (v / (double) c.size());
}
} // namespace vox148

TEST (lynx_tune_held_note_in_noise_round148)
{
    using namespace lynx;
    // [EXEC] Auto-Tune Pro (official trial, retune 0 and 50, chromatic): held notes with white noise 20 dB under them
    // come out within 0.1–0.6 c rms; LYNX Tune wobbled 2–5 c on the low notes (the marks jumped between noise peaks).
    // Now the marks follow the waveform's repetition: a 110 Hz and a 220 Hz note in noise stay within about a cent.
    for (const double hz : { 110.0, 220.0 })
        for (const float retune : { 0.0f, 0.5f })
        {
            auto slot = makeEffectSlot (EffectKind::lynxTune);
            slot.params[lynxtune::scale] = 0.0f; // chromatic (the notes are in tune)
            slot.params[lynxtune::retune] = retune;
            auto fx = makeEffect (slot);
            const double sr = 48000.0;
            fx->prepare (sr);
            auto l = vox148::noisyNote (hz, 20.0, 2.0, sr), r = l;
            const int n = (int) l.size();
            for (int i = 0; i < n; i += 256)
                fx->process (&l[(std::size_t) i], &r[(std::size_t) i], std::min (256, n - i), slot.params, 120.0);
            const auto dry = vox148::wobbleCents (vox148::noisyNote (hz, 20.0, 2.0, sr), hz, sr, 0.5, 1.9);
            const auto wet = vox148::wobbleCents (l, hz, sr, 0.5, 1.9);
            std::printf ("  lynx tune %.0f Hz retune %.0f ms, noise 20 dB under: wobble %.2f c (dry %.2f c)\n", hz,
                         lynxtune::retuneMs (retune), wet, dry);
            CHECK (wet < 1.2);
        }
}

// Round 148 §2: Ridge Strip's 4K voicing — the console measured on the SSL 4K E trial (core/VoiceMatch.h, model 1)
namespace vox148
{
lynx::EffectParams k4 ()
{
    namespace r = lynx::mixsuite::ridge;
    auto p = lynx::makeEffectSlot (lynx::EffectKind::ridgeStrip).params;
    p[r::console4k] = 1.0f;
    p[r::analog] = 0.0f;
    p[r::autoMakeup] = 0.0f;
    return p;
}

// steady level change (dB) of a 1 kHz sine through Ridge over the last 0.1 s of `sec` seconds (the 4K's slow attack)
double gainDb (const lynx::EffectParams& p, double dbfs, double sec = 2.5)
{
    const double sr = 48000.0;
    const auto in = mixsuite96::sine (1000.0, std::pow (10.0, dbfs / 20.0), (int) (sec * sr), sr);
    const auto out = mixsuite96::run (lynx::EffectKind::ridgeStrip, p, in, sr);
    double a = 0, b = 0;
    for (std::size_t i = in.size() - (std::size_t) (0.1 * sr); i < in.size(); ++i)
    {
        a += (double) in[i] * in[i];
        b += (double) out[i] * out[i];
    }
    return 10.0 * std::log10 (b / a);
}
} // namespace vox148

TEST (ridge_4k_voicing_eq_round148)
{
    using namespace lynx;
    namespace e = vmatch::echannel;
    namespace r = mixsuite::ridge;
    // [EXEC] the trial's impulse responses at 48 kHz (VCA and auto make-up off): the same dial, three colours
    auto band = [] (int colour, double lmfDb) {
        e::Settings s;
        s.model = 1;
        s.colour = colour;
        s.lmfGain = lmfDb; s.lmfKHz = 1.0; s.lmfQ = 1.5;
        s.lfGain = s.hmfGain = s.hfGain = 0.0;
        return s;
    };
    CHECK (std::abs (e::responseDb (band (0, 10.0), 48000.0, 1038.0) - 7.77) < 0.15); // Brown
    CHECK (std::abs (e::responseDb (band (1, 10.0), 48000.0, 818.0) - 8.43) < 0.15);  // Black: lower, sharper
    CHECK (std::abs (e::responseDb (band (2, 10.0), 48000.0, 998.0) - 11.50) < 0.15); // Orange
    e::Settings hf;
    hf.model = 1; hf.colour = 1; hf.lmfGain = hf.hmfGain = hf.lfGain = 0.0; hf.hfGain = 15.0; hf.hfKHz = 8.0;
    CHECK (std::abs (e::responseDb (hf, 48000.0, 16000.0) - 15.49) < 0.3);
    e::Settings lf;
    lf.model = 1; lf.colour = 0; lf.lmfGain = lf.hmfGain = lf.hfGain = 0.0; lf.lfGain = -10.0; lf.lfHz = 100.0;
    CHECK (std::abs (e::responseDb (lf, 48000.0, 30.0) + 7.57) < 0.2);
    e::Settings f;
    f.model = 1; f.lmfGain = f.hmfGain = f.hfGain = f.lfGain = 0.0; f.hpHz = 100.0;
    CHECK (std::abs (e::responseDb (f, 48000.0, 50.0) + 6.49) < 0.2 && std::abs (e::responseDb (f, 48000.0, 200.0)) < 0.2);
    f.hpHz = 0.0; f.lpKHz = 5.0;
    CHECK (std::abs (e::responseDb (f, 48000.0, 10000.0) + 9.78) < 0.3);
    // Ridge's own scales in the voicing: the 4K's Q dial 0.5 … 3 and its colours
    CHECK (std::abs (r::k4Q (0.0f) - 0.5) < 1e-9 && std::abs (r::k4Q (1.0f) - 3.0) < 1e-9 && std::abs (r::k4Q (r::k4QValue (2.2)) - 2.2) < 1e-4);
    CHECK (r::k4ColourIndex (r::k4ColourValue (2)) == 2 && r::k4ColourIndex (0.0f) == 0);
    CHECK (formatEffectParam (EffectKind::ridgeStrip, r::k4Colour, 1.0f, 120.0, vox148::k4()) == "orange");
    CHECK (formatEffectParam (EffectKind::ridgeStrip, r::lmfQ, 1.0f, 120.0, vox148::k4()) == "Q 3.00");
}

TEST (ridge_4k_voicing_dynamics_round148)
{
    using namespace lynx;
    namespace r = mixsuite::ridge;
    auto comp = [] (double ratio, double thr, bool makeup) {
        auto p = vox148::k4();
        p[r::compRatio] = (float) std::sqrt ((ratio - 1.0) / 19.0);
        p[r::compThreshold] = (float) ((thr + 20.0) / 30.0);
        p[r::autoMakeup] = makeup ? 1.0f : 0.0f;
        return p;
    };
    // [EXEC] 1 kHz stairs on the trial, 5:1 at -10 (console scale): -15.5 dB at 0 dBFS, nothing at -36 dBFS; the
    // automatic make-up +3.37 dB below the threshold
    const auto full = vox148::gainDb (comp (5.0, -10.0, false), 0.0);
    std::printf ("  4K 5:1 at -10: %.2f dB at 0 dBFS (trial -15.5)\n", full);
    CHECK (std::abs (full + 15.5) < 1.2);
    CHECK (std::abs (vox148::gainDb (comp (5.0, -10.0, false), -36.0)) < 0.1);
    CHECK (std::abs (vox148::gainDb (comp (5.0, -10.0, true), -50.0) - 3.37) < 0.1);
    // the mic pre: Mic 40 dB with the input trim at -20 dB, 1 kHz at 0 dBFS: [EXEC] the trial -35.6 dB (rms, incl. the trim)
    auto mic = vox148::k4();
    mic[r::k4Pre] = 1.0f;
    mic[r::k4Mic] = 0.8f;
    mic[r::input] = 0.0f;
    const auto pre = vox148::gainDb (mic, 0.0, 0.5);
    CHECK (std::abs (pre + 35.6) < 0.6);
    CHECK (std::abs (vox148::gainDb (mic, -40.0, 0.5) + 20.27) < 0.1); // clean below (Pre in: -0.27 dB)
    // the Analogue VCA leaves the level of a 1 kHz tone alone
    auto vca = vox148::k4();
    vca[r::analog] = 1.0f;
    CHECK (std::abs (vox148::gainDb (vca, -10.0, 0.5)) < 0.05);
}

TEST (ridge_4k_state_translated_round148)
{
    using namespace lynx;
    namespace r = mixsuite::ridge;
    // an SSL 4K E state (XML in the original's units; the A setting plays): Orange, LF bell 75 Hz -4.5 dB, LMF 0.65 kHz
    // +3.5 dB Q 2.2, HPF 85 Hz, LPF 9 kHz, 23.3:1 at -7.5, release 1.11 s, mix 70 %, make-up offset +3, the expander,
    // mic pre 22 dB, trims -3 / +2.5 with the fader at -4, dynamics before the EQ, polarity
    const std::string a = "<PARAM id=\"AnalogueVca\" value=\"0.0\"/><PARAM id=\"CompAutoMakeUp\" value=\"1.0\"/>"
                          "<PARAM id=\"CompAutoMakeUpCompensation\" value=\"3.0\"/><PARAM id=\"CompFastAttack\" value=\"1.0\"/>"
                          "<PARAM id=\"CompMix\" value=\"70.0\"/><PARAM id=\"CompRatio\" value=\"23.3\"/><PARAM id=\"CompRelease\" value=\"1.11\"/>"
                          "<PARAM id=\"CompThreshold\" value=\"-7.5\"/><PARAM id=\"DynamicsIn\" value=\"1.0\"/><PARAM id=\"DynamicsPreEq\" value=\"1.0\"/>"
                          "<PARAM id=\"EqColour\" value=\"2.0\"/><PARAM id=\"EqIn\" value=\"1.0\"/><PARAM id=\"FaderLevel\" value=\"-4.0\"/>"
                          "<PARAM id=\"GateExpander\" value=\"1.0\"/><PARAM id=\"GateRange\" value=\"15.0\"/><PARAM id=\"GateRelease\" value=\"0.7\"/>"
                          "<PARAM id=\"GateThreshold\" value=\"-18.0\"/><PARAM id=\"HighEqFreq\" value=\"11.0\"/><PARAM id=\"HighEqGain\" value=\"7.0\"/>"
                          "<PARAM id=\"HighMidEqFreq\" value=\"4.2\"/><PARAM id=\"HighMidEqGain\" value=\"-6.0\"/><PARAM id=\"HighMidEqQ\" value=\"0.8\"/>"
                          "<PARAM id=\"HighPassFreq\" value=\"85.0\"/><PARAM id=\"InputTrim\" value=\"-3.0\"/><PARAM id=\"LowEqBell\" value=\"1.0\"/>"
                          "<PARAM id=\"LowEqFreq\" value=\"75.0\"/><PARAM id=\"LowEqGain\" value=\"-4.5\"/><PARAM id=\"LowMidEqFreq\" value=\"0.65\"/>"
                          "<PARAM id=\"LowMidEqGain\" value=\"3.5\"/><PARAM id=\"LowMidEqQ\" value=\"2.2\"/><PARAM id=\"LowPassFreq\" value=\"9.0\"/>"
                          "<PARAM id=\"OutputTrim\" value=\"2.5\"/><PARAM id=\"PhaseInvert\" value=\"1.0\"/><PARAM id=\"SaturationAmount\" value=\"22.0\"/>"
                          "<PARAM id=\"SaturationIn\" value=\"1.0\"/><PARAM id=\"Bypass\" value=\"0.0\"/>";
    ForeignPlugin fp;
    fp.format = ForeignPlugin::Format::vst3;
    fp.name = "SSL 4K E";
    fp.state = substtest::flVst3State (substtest::juceXmlComponent (
        "<SSL_PLUGIN_STATE PluginName=\"SSL 4K E\" Version=\"1.7.1\" StateASelected=\"1\"><A><PROCESSOR_STATE>" + a
        + "</PROCESSOR_STATE></A><B><PROCESSOR_STATE><PARAM id=\"CompRatio\" value=\"1.0\"/></PROCESSOR_STATE></B></SSL_PLUGIN_STATE>"));
    const auto id = pluginIdentity (fp);
    const auto* entry = findPluginSubstitute (id, false);
    CHECK (entry != nullptr);
    if (entry == nullptr)
        return;
    int n = 0;
    const auto slot = makeSubstituteSlot (*entry, &id, &n);
    const auto& p = slot.params;
    CHECK (slot.kind == EffectKind::ridgeStrip && n >= 30 && r::k4Voicing (p) && ! r::vocalVoicing (p));
    CHECK (r::k4ColourIndex (p[r::k4Colour]) == 2 && p[r::lfBell] > 0.5f && p[r::order] > 0.5f && p[r::phase] > 0.5f);
    CHECK (std::abs (r::lfHz (p[r::lfFreq]) - 75.0) < 0.01 && std::abs (r::bandGainDb (p[r::lfGain], false) + 4.5) < 1e-4);
    CHECK (std::abs (r::lmfHz (p[r::lmfFreq]) - 650.0) < 0.1 && std::abs (r::k4Q (p[r::lmfQ]) - 2.2) < 1e-3);
    CHECK (std::abs (r::hpfHz (p[r::hpf]) - 85.0) < 0.01 && std::abs (r::lpfHz (p[r::lpf]) - 9000.0) < 1.0);
    CHECK (r::ratio (p[r::compRatio]) > 19.9 && std::abs (r::eCompThresholdDb (p[r::compThreshold]) + 7.5) < 1e-4);
    CHECK (std::abs (r::releaseSeconds (p[r::compRelease]) - 1.11) < 1e-4 && std::abs (p[r::compMix] - 0.7f) < 1e-6f);
    CHECK (p[r::autoMakeup] > 0.5f && std::abs (r::k4OffsetDb (p[r::k4MakeupOffset]) - 3.0) < 1e-4 && p[r::compFastAttack] > 0.5f);
    CHECK (p[r::expander] > 0.5f && std::abs (r::eGateRangeDb (p[r::gateRange]) - 15.0) < 1e-4 && std::abs (r::eGateThresholdDb (p[r::gateThreshold]) + 18.0) < 1e-4);
    CHECK (p[r::k4Pre] > 0.5f && std::abs (r::k4MicDb (p[r::k4Mic]) - 22.0) < 1e-4 && p[r::analog] < 0.5f);
    CHECK (std::abs (r::trimDb (p[r::input]) + 3.0) < 1e-4 && std::abs (r::trimDb (p[r::output]) + 1.5) < 1e-4); // +2.5 - 4
}

// Round 148 §3: FabFilter Pro-DS (her two states) -> Hush De-esser's measured vocal voicing
namespace vox148
{
// a "FabF" version-2 component state named "Default Setting" holding `values`
inline std::vector<std::uint8_t> fabf (const std::vector<float>& values)
{
    std::vector<std::uint8_t> b { 'F', 'a', 'b', 'F' };
    auto put = [&b] (std::uint32_t u) {
        for (int i = 0; i < 4; ++i)
            b.push_back ((std::uint8_t) (u >> (8 * i)));
    };
    const std::string name = "Default Setting";
    put (2);
    put ((std::uint32_t) name.size());
    b.insert (b.end(), name.begin(), name.end());
    put (0);
    put ((std::uint32_t) values.size());
    for (const auto v : values)
    {
        std::uint32_t u;
        std::memcpy (&u, &v, 4);
        put (u);
    }
    put (1); put (1); // (two trailing words, as her states carry)
    return b;
}
// [DATA] her first Pro-DS state: Single Vocal, threshold -43.71 dB, range 6 dB, wide band, look-ahead 12 ms on,
// band 7 … 14 kHz
const std::vector<float> herProDs { 0.0f, 0.2715f, 6.0f, 0.0f, 0.5f, 0.0f, 12.0f, 1.0f, 0.0f, 0.0f, 0.54407f, 0.8451f,
                                    0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
} // namespace vox148

TEST (pro_ds_translates_onto_hush_vocal_voicing_round148)
{
    using namespace lynx;
    namespace h = mixsuite::hush;
    int n = 0;
    const auto slot = m124x::substitute ("FabFilter Pro-DS", vox148::fabf (vox148::herProDs), n);
    const auto& p = slot.params;
    CHECK (slot.kind == EffectKind::hushDeEsser && n == 20 && h::vocal (p));
    CHECK (std::abs (h::thresholdDb (p[h::threshold]) + 43.71) < 0.01 && std::abs (h::rangeDb (p[h::range]) - 6.0) < 1e-4);
    CHECK (std::abs (h::hpHz (p[h::frequency], h::extended (p)) - 7000.0) < 2.0 && std::abs (h::lowPassHz (p[h::lowPass]) - 14000.0) < 5.0);
    CHECK (p[h::mode] > 0.5f && p[h::lookahead] > 0.5f && std::abs (h::lookaheadTimeMs (p[h::lookaheadTime]) - 12.0) < 0.01);
    // Allround keeps Hush's own engine in its allround mode
    auto all = vox148::herProDs;
    all[0] = 1.0f;
    const auto a = m124x::substitute ("FabFilter Pro-DS", vox148::fabf (all), n);
    CHECK (! h::vocal (a.params) && a.params[h::allround] > 0.5f);

    // [EXEC] on the trial at her state: latency 720 samples at 48 kHz; a steady 8 kHz tone at -10 dBFS is turned down by
    // 5.2 dB (range x (L - thr + 4.1) / (0.1 - thr)); pink noise never triggers it
    auto fx = makeEffect (slot);
    fx->prepare (48000.0);
    CHECK (fx->latencySamples() == 720);
    const int len = 48000;
    std::vector<float> l ((std::size_t) len), r;
    for (int i = 0; i < len; ++i)
        l[(std::size_t) i] = (float) (std::pow (10.0, -0.5) * std::sin (2.0 * 3.141592653589793 * 8000.0 * i / 48000.0));
    r = l;
    const auto in = l;
    for (int i = 0; i < len; i += 256)
        fx->process (&l[(std::size_t) i], &r[(std::size_t) i], std::min (256, len - i), slot.params, 120.0);
    double a2 = 0.0, b2 = 0.0;
    for (int i = len - 9600; i < len; ++i) { a2 += (double) in[(std::size_t) (i - 720)] * in[(std::size_t) (i - 720)]; b2 += (double) l[(std::size_t) i] * l[(std::size_t) i]; }
    const auto gr = 10.0 * std::log10 (b2 / a2);
    std::printf ("  Hush vocal voicing, 8 kHz at -10 dBFS: %.2f dB (trial -5.2)\n", gr);
    CHECK (std::abs (gr + 5.2) < 0.4);
    fx->reset();
    std::uint32_t st = 148;
    double prev = 0.0;
    for (int i = 0; i < len; ++i) // a rough pink (one-pole on white), -20 dBFS-ish
    {
        st = st * 1664525u + 1013904223u;
        prev = 0.97 * prev + 0.03 * ((double) (st >> 8) / 8388608.0 - 1.0);
        l[(std::size_t) i] = r[(std::size_t) i] = (float) (prev * 1.5);
    }
    const auto pinkIn = l;
    for (int i = 0; i < len; i += 256)
        fx->process (&l[(std::size_t) i], &r[(std::size_t) i], std::min (256, len - i), slot.params, 120.0);
    double e = 0.0, s = 0.0;
    for (int i = 4800; i < len; ++i) { const auto d = l[(std::size_t) i] - pinkIn[(std::size_t) (i - 720)]; e += d * d; s += (double) pinkIn[(std::size_t) (i - 720)] * pinkIn[(std::size_t) (i - 720)]; }
    CHECK (10.0 * std::log10 (e / s + 1e-30) < -45.0); // (at most a trace: -52.7 dB, 0.09 dB of reduction)
}

// Round 148 §4: Echoform's ping-pong tape voicing plays EchoBoy's measured kernels
TEST (echoform_ping_pong_kernels_round148)
{
    using namespace lynx;
    namespace e = echosuite::echoform;
    // [EXEC] her EchoBoy ping-pong state (1/4 note, feedback 0.52 = loop gain 0.356) at 120 BPM: a left impulse of 0.003
    // comes back on the left one beat + 38 samples later (its tap 0.389 of the summed input, with the diffusion around
    // it) and on the right two beats + 43 later; the first pass now IS the measured kernel
    auto slot = makeEffectSlot (EffectKind::echoform);
    slot.params[(size_t) e::tapeVoicing] = e::tapeVoicingValue (1);
    slot.params[(size_t) e::syncL] = slot.params[(size_t) e::syncR] = 0.625f;
    slot.params[(size_t) e::noteL] = slot.params[(size_t) e::noteR] = 0.45f;
    slot.params[(size_t) e::feedback] = 0.178f;
    slot.params[(size_t) e::mix] = 1.0f;
    slot.params[(size_t) e::output] = 0.5f;
    auto fx = makeEffect (slot);
    fx->prepare (48000.0);
    const int len = 60000;
    std::vector<float> l ((std::size_t) len, 0.0f), r ((std::size_t) len, 0.0f);
    l[0] = 0.003f;
    for (int off = 0; off < len; off += 512)
        fx->process (l.data() + off, r.data() + off, std::min (512, len - off), slot.params, 120.0);
    int pl = 0, pr = 0;
    for (int i = 1; i < len; ++i)
    {
        if (std::abs (l[(std::size_t) i]) > std::abs (l[(std::size_t) pl])) pl = i;
        if (std::abs (r[(std::size_t) i]) > std::abs (r[(std::size_t) pr])) pr = i;
    }
    std::printf ("  ping-pong peaks: L %d (%.3f), R %d (%.3f)\n", pl, l[(std::size_t) pl] / 0.003f, pr, r[(std::size_t) pr] / 0.003f);
    CHECK (pl == 24038 && pr == 48043);
    CHECK (std::abs (l[(std::size_t) pl] / 0.003f - 0.389f) < 0.02f);
    // the left pass around its tap: diffusion taps 300 … 641 samples either side (none in a plain tap + colour)
    double around = 0.0, main = 0.0;
    for (int i = pl - 700; i <= pl + 700; ++i)
        (std::abs (i - pl) >= 290 ? around : main) += (double) l[(std::size_t) i] * l[(std::size_t) i];
    CHECK (around > 0.05 * main);
}
