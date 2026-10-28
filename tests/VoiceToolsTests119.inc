// Round 119: LYNX's voice tools (Quell Denoiser, Gemini Doubler), Echoform's output / pairs, and the Waves H-Delay /
// NS1 / Doubler translations (docs/analise/REA_rodada119.md).


#include "core/EchoSuite.h"
#include "core/VoiceTools.h"
namespace vt119
{
constexpr double sr = 48000.0;

struct Stereo
{
    std::vector<float> l, r;
};

inline Stereo run (lynx::EffectKind kind, const lynx::EffectParams& p, Stereo x)
{
    auto slot = lynx::makeEffectSlot (kind);
    slot.params = p;
    auto fx = lynx::makeEffect (slot);
    fx->prepare (sr);
    fx->reset();
    for (std::size_t off = 0; off < x.l.size(); off += 256)
    {
        const auto m = (int) std::min<std::size_t> (256, x.l.size() - off);
        fx->process (x.l.data() + off, x.r.data() + off, m, p, 120.0);
    }
    return x;
}

inline double rmsDb (const std::vector<float>& v, std::size_t from, std::size_t to)
{
    double e = 0.0;
    for (auto i = from; i < to && i < v.size(); ++i)
        e += (double) v[i] * v[i];
    return 10.0 * std::log10 (e / (double) std::max<std::size_t> (1, to - from) + 1e-30);
}

inline Stereo tone (double hz, double db, double secs)
{
    Stereo s;
    const auto n = (std::size_t) (secs * sr);
    s.l.resize (n);
    for (std::size_t i = 0; i < n; ++i)
        s.l[i] = (float) (std::pow (10.0, db / 20.0) * std::sin (2.0 * 3.141592653589793 * hz * (double) i / sr));
    s.r = s.l;
    return s;
}

inline std::size_t peakAt (const std::vector<float>& v, std::size_t from)
{
    std::size_t best = from;
    for (auto i = from; i < v.size(); ++i)
        if (std::abs (v[i]) > std::abs (v[best]))
            best = i;
    return best;
}

// the strongest frequency (1 Hz grid, 300 .. 1500 Hz) over [from, to): a DFT scan
inline double frequency (const std::vector<float>& v, std::size_t from, std::size_t to)
{
    double best = 0.0, bestHz = 0.0;
    for (int hz = 300; hz <= 1500; ++hz)
    {
        double re = 0.0, im = 0.0;
        const auto w = 2.0 * 3.141592653589793 * hz / sr;
        for (auto i = from; i < to; i += 2)
        {
            re += v[i] * std::cos (w * (double) i);
            im += v[i] * std::sin (w * (double) i);
        }
        if (re * re + im * im > best)
            best = re * re + im * im, bestHz = hz;
    }
    return bestHz;
}
} // namespace vt119

TEST (quell_denoiser_cuts_quiet_noise_and_keeps_the_voice)
{
    using namespace lynx;
    namespace q = voicetools::quell;
    auto p = makeEffectSlot (EffectKind::quellDenoiser).params;
    p[q::amount] = 0.23f; // her most common setting
    // a loud tone passes within 1 dB; the same tone 60 dB lower is pulled down by more than 12 dB
    for (const auto hz : { 250.0, 1000.0, 4000.0 })
    {
        const auto loud = vt119::tone (hz, -12.0, 1.0);
        const auto y = vt119::run (EffectKind::quellDenoiser, p, loud);
        CHECK (std::abs (vt119::rmsDb (y.l, 24000, 48000) - vt119::rmsDb (loud.l, 24000, 48000)) < 1.0);
        const auto quiet = vt119::tone (hz, -78.0, 1.0);
        const auto z = vt119::run (EffectKind::quellDenoiser, p, quiet);
        CHECK (vt119::rmsDb (z.l, 24000, 48000) - vt119::rmsDb (quiet.l, 24000, 48000) < -12.0);
    }
    // the band split is phase coherent: with nothing to reduce (the static law at 0 dB) the bands sum back to the input
    // (an all-pass: same energy)
    voicetools::quell::Tuning flat = q::defaultTuning();
    for (int b = 0; b < q::bands; ++b)
    {
        flat.range0[b] = flat.range1[b] = 0.0;
        flat.floorCut0[b] = flat.floorCut1[b] = 0.0;
    }
    auto fx = q::make (flat);
    fx->prepare (vt119::sr);
    auto x = vt119::tone (3000.0, -20.0, 0.5);
    const auto in = x;
    fx->process (x.l.data(), x.r.data(), (int) x.l.size(), p, 120.0);
    CHECK (std::abs (vt119::rmsDb (x.l, 12000, 24000) - vt119::rmsDb (in.l, 12000, 24000)) < 0.05);
    // the reduction law: none far above the threshold, the full depth far below it, more Amount = more reduction
    const auto& t = q::defaultTuning();
    CHECK (q::reductionDb (t, 3, 0.0, 0.23) == t.floorCut0[3] + (t.floorCut1[3] - t.floorCut0[3]) * 0.23);
    CHECK (q::reductionDb (t, 3, -120.0, 0.23) > 15.0);
    CHECK (q::reductionDb (t, 3, -60.0, 0.7) >= q::reductionDb (t, 3, -60.0, 0.23));
    // listen: what is taken away (a quiet tone: most of it)
    p[q::listen] = 1.0f;
    const auto quiet = vt119::tone (1000.0, -78.0, 1.0);
    const auto removed = vt119::run (EffectKind::quellDenoiser, p, quiet);
    CHECK (vt119::rmsDb (removed.l, 24000, 48000) - vt119::rmsDb (quiet.l, 24000, 48000) > -1.5);
}

TEST (gemini_doubler_voices_follow_the_measured_laws)
{
    using namespace lynx;
    namespace g = voicetools::gemini;
    auto base = makeEffectSlot (EffectKind::geminiDoubler).params;
    base[g::lowOn] = base[g::highOn] = 0.0f; // round 165: new instances start with the reference's shelves on
    // one voice alone, source L+R, no detune, 10 ms, panned centre, not aligned: an impulse comes back 10 ms later
    // at −3 dB on each side (equal-power pan)
    auto p = base;
    p[g::directOn] = 0.0f;
    p[g::align] = 0.0f;
    p[(std::size_t) g::voiceParam (1, g::vOn)] = 0.0f;
    p[(std::size_t) g::voiceParam (0, g::vGain)] = 1.0f;
    p[(std::size_t) g::voiceParam (0, g::vPan)] = 0.5f;
    p[(std::size_t) g::voiceParam (0, g::vDelay)] = 0.1f;
    p[(std::size_t) g::voiceParam (0, g::vDetune)] = 0.5f;
    p[(std::size_t) g::voiceParam (0, g::vSource)] = 0.5f;
    vt119::Stereo imp { std::vector<float> (9600, 0.0f), std::vector<float> (9600, 0.0f) };
    imp.l[100] = imp.r[100] = 0.5f;
    auto y = vt119::run (EffectKind::geminiDoubler, p, imp);
    const auto at = vt119::peakAt (y.l, 101);
    const auto expected = 100.0 + 0.010 * vt119::sr;
    CHECK (std::abs ((double) at - expected) <= 2.0);
    CHECK (std::abs (20.0 * std::log10 (std::abs (y.l[at]) / 0.5) + 3.01) < 0.3 && std::abs (y.l[at] - y.r[at]) < 1e-5f);
    // hard left (−45°): nothing on the right
    p[(std::size_t) g::voiceParam (0, g::vPan)] = 0.0f;
    y = vt119::run (EffectKind::geminiDoubler, p, imp);
    CHECK (vt119::rmsDb (y.r, 0, y.r.size()) < -150.0);
    // detune: +50 cents on a 1 kHz tone
    p[(std::size_t) g::voiceParam (0, g::vPan)] = 0.5f;
    p[(std::size_t) g::voiceParam (0, g::vDetune)] = 0.75f;
    auto t = vt119::run (EffectKind::geminiDoubler, p, vt119::tone (1000.0, -20.0, 1.0));
    CHECK (std::abs (1200.0 * std::log2 (vt119::frequency (t.l, 9600, 48000) / 1000.0) - 50.0) < 3.5);
    // the octave switch: one octave down (the two-grain shifter puts its energy on 75 Hz lines around 500 Hz: the
    // strongest is within 30 Hz; the reference is pitch-synchronous and cleaner — REA_rodada119 §4)
    p[(std::size_t) g::voiceParam (0, g::vDetune)] = 0.5f;
    p[(std::size_t) g::voiceParam (0, g::vOctave)] = 1.0f;
    t = vt119::run (EffectKind::geminiDoubler, p, vt119::tone (1000.0, -20.0, 1.0));
    CHECK (std::abs (vt119::frequency (t.l, 9600, 48000) - 500.0) <= 30.0);
    // feedback 39 %: each repeat 8.2 dB down (no detune)
    p[(std::size_t) g::voiceParam (0, g::vOctave)] = 0.0f;
    p[(std::size_t) g::voiceParam (0, g::vFeedback)] = 0.39f;
    p[(std::size_t) g::voiceParam (0, g::vDelay)] = 0.5f;
    y = vt119::run (EffectKind::geminiDoubler, p, imp);
    const auto e1 = vt119::peakAt (y.l, 101), e2 = vt119::peakAt (y.l, e1 + 1000);
    CHECK (std::abs ((double) (e2 - e1) - 0.050 * vt119::sr) <= 2.0);
    CHECK (std::abs (20.0 * std::log10 (std::abs (y.l[e2] / y.l[e1])) + 8.18) < 0.3);
    // the direct signal: pan rotates the stereo field (−45°: L' = (L + R)/√2, R' = (R − L)/√2) and Align delays it
    p = base;
    p[(std::size_t) g::voiceParam (0, g::vOn)] = 0.0f;
    p[(std::size_t) g::voiceParam (1, g::vOn)] = 0.0f;
    p[g::align] = 0.0f;
    p[g::directPan] = 0.0f;
    vt119::Stereo left { std::vector<float> (2000, 0.0f), std::vector<float> (2000, 0.0f) };
    left.l[10] = 1.0f;
    y = vt119::run (EffectKind::geminiDoubler, p, left);
    CHECK (std::abs (y.l[10] - 0.70710678f) < 1e-4f && std::abs (y.r[10] + 0.70710678f) < 1e-4f);
    p[g::directPan] = 0.5f;
    p[g::align] = 1.0f;
    y = vt119::run (EffectKind::geminiDoubler, p, left);
    CHECK (vt119::peakAt (y.l, 0) == 10 + 320 && std::abs (y.l[330] - 1.0f) < 1e-4f);
}

TEST (echoform_pairs_and_output_round119)
{
    using namespace lynx;
    namespace e = echosuite::echoform;
    auto p = makeEffectSlot (EffectKind::echoform).params;
    CHECK (p[e::output] == 0.5f && p[e::pairs] == 0.0f); // older projects load these defaults: unchanged sound
    p[e::mix] = 1.0f;
    p[e::style] = e::styleValue (e::Style::pingPong);
    p[e::syncL] = p[e::syncR] = 0.0f;
    p[e::timeL] = p[e::timeR] = e::timeValue (100.0);
    p[e::mode] = e::modeValue (e::Mode::clean);
    p[e::age] = 0.0f;
    p[e::modDepth] = 0.0f;
    p[e::feedback] = 0.18f; // loop gain 0.36
    p[e::pairs] = 1.0f;
    p[e::output] = (6.0f + 18.0f) / 36.0f;
    vt119::Stereo imp { std::vector<float> (48000, 0.0f), std::vector<float> (48000, 0.0f) };
    imp.l[0] = imp.r[0] = 0.01f;
    const auto y = vt119::run (EffectKind::echoform, p, imp);
    auto level = [&] (const std::vector<float>& v, double ms) {
        const auto c = (std::size_t) (ms * 48.0);
        float m = 0.0f;
        for (auto i = c - 200; i < c + 200; ++i)
            m = std::max (m, std::abs (v[i]));
        return 20.0 * std::log10 (m / 0.01);
    };
    // L at 100 ms (+6 dB output), R at 200 ms at the same level, then the loop gain once per pair
    CHECK (std::abs (level (y.l, 100.0) - 6.0) < 0.5 && std::abs (level (y.r, 200.0) - level (y.l, 100.0)) < 0.5);
    CHECK (std::abs (level (y.l, 300.0) - level (y.l, 100.0) - 20.0 * std::log10 (0.36)) < 0.6);
}

TEST (waves_h_delay_ns1_doubler_translate_round119)
{
    using namespace lynx;
    auto slotFor = [] (const char* waves, const std::string& values, int& translated) {
        PluginIdentity id;
        id.format = ForeignPlugin::Format::vst3;
        id.name = "WaveShell1-VST3 16.0_x64";
        id.state = substtest::wavesComponent (waves, values);
        const auto* entry = findPluginSubstitute (id, false);
        CHECK (entry != nullptr);
        return entry != nullptr ? makeSubstituteSlot (*entry, &id, &translated) : EffectSlot {};
    };
    // her main H-Delay: host sync 1/2, feedback 60, ping-pong, mix 100, analog Mod2
    {
        int n = 0;
        const auto s = slotFor ("H-Delay", "* 1 120 1000 16 * 100 60 0 0 1 * * * 0 20100 * 19 * 0 0.1 0 * 2", n);
        namespace e = echosuite::echoform;
        CHECK (n > 0 && s.kind == EffectKind::echoform);
        CHECK (e::styleOf (s.params[e::style]) == e::Style::pingPong && s.params[e::pairs] == 1.0f);
        CHECK (std::abs (e::sideMs (s.params, false, 120.0) - 1000.0) < 0.01 && std::abs (e::sideMs (s.params, true, 90.0) - 60000.0 / 90.0 * 2.0) < 0.01);
        // round 124: the hybrid engine (analog), H-Delay's own feedback (0.6) and output (0 dB)
        CHECK (e::hybridOf (s.params[e::hybrid]) == 2);
        CHECK (std::abs (e::feedbackGain (s.params[e::feedback]) - 0.6) < 1e-3);
        CHECK (s.params[e::mix] == 1.0f && std::abs (e::outputDb (s.params[e::output])) < 0.01);
    }
    // its own BPM (1/8 dotted at 125 = 360 ms), stereo mode, +8.2 dB, mix 41
    {
        int n = 0;
        const auto s = slotFor ("H-Delay", "* 0 125 360 11 * 41 40 0 0 0 * * * 8.2 20100 * 19 * 0 0.1 0 * 0", n);
        namespace e = echosuite::echoform;
        CHECK (e::styleOf (s.params[e::style]) == e::Style::single && s.params[e::pairs] == 0.0f);
        CHECK (std::abs (e::sideMs (s.params, false, 90.0) - 360.0) < 2.0);
        CHECK (e::hybridOf (s.params[e::hybrid]) == 1); // Analog off
        CHECK (std::abs (e::feedbackGain (s.params[e::feedback]) - 0.4) < 1e-3);
        CHECK (std::abs (s.params[e::mix] - 0.41f) < 1e-4f && std::abs (e::outputDb (s.params[e::output]) - 8.2) < 0.01);
    }
    // NS1: value 229 is the knob
    {
        std::string values;
        for (int i = 0; i < 240; ++i)
            values += i == 229 ? "23.1 " : "0 ";
        int n = 0;
        const auto s = slotFor ("NS1", values, n);
        CHECK (n == 2 && s.kind == EffectKind::quellDenoiser && std::abs (s.params[voicetools::quell::amount] - 0.231f) < 1e-5f);
        CHECK (s.params[voicetools::quell::matched] == 1.0f); // round 124: the engine fitted to the trial
    }
    // her Doubler: voices 78.2 / 100 ms, feedback 39 %, −1 / 0 cents, hard left / right, direct aligned, EQ off
    {
        int n = 0;
        const auto s = slotFor ("Doubler", "0 1 0 0 1 * * * * 0 -28.7 841 3538 -13.7 0 16 0 0 0 0 64 20 32 -60 -40 -20 1 0 -45 0 -1 78.2 39 0 1 * "
                                           "1 0 45 0 0 100 39 0 1 * 0 0 -45 0 0 100 0 0 1 * 0 0 45 0 0 100 0 0 1 * * * *", n);
        namespace g = voicetools::gemini;
        CHECK (n == 33 && s.kind == EffectKind::geminiDoubler);
        const auto v = [&] (int k, g::VoiceParam q) { return s.params[(std::size_t) g::voiceParam (k, q)]; };
        CHECK (std::abs (g::delayMs (v (0, g::vDelay)) - 78.2) < 0.01 && std::abs (g::delayMs (v (1, g::vDelay)) - 100.0) < 0.01);
        CHECK (std::abs (g::feedback (v (0, g::vFeedback)) - 0.39) < 1e-4 && std::abs (g::detuneCents (v (0, g::vDetune)) + 1.0) < 0.01);
        CHECK (g::panDegrees (v (0, g::vPan)) == -45.0 && g::panDegrees (v (1, g::vPan)) == 45.0);
        CHECK (s.params[g::align] == 1.0f && s.params[g::directOn] == 1.0f && s.params[g::lowOn] == 0.0f && s.params[g::highOn] == 0.0f);
        CHECK (g::sourceOf (v (0, g::vSource)) == 0 && g::sourceOf (v (1, g::vSource)) == 0 && std::abs (g::rateValue (v (0, g::vRate)) - 1.0) < 1e-3);
        CHECK (std::abs (g::outputDb (s.params[g::output])) < 1e-6);
    }
}
