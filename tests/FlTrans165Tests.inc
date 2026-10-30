// --- Transmitter (core/Transmitter.h) - round 165g, sub-round fltrans165 (REA_rodada165g-fltrans165.md)

#include "core/Transmitter.h"

namespace fltrans165
{
// FL's state 0xD5: version 3, the 42 raw controls, transient / sustain send tracks, sidechain input, editor value + bytes
std::vector<std::uint8_t> flState (const std::array<int, lynx::transmitter::count>& raw, int transientSend, int sustainSend, int sidechain)
{
    std::vector<std::uint8_t> out;
    auto put = [&out] (std::int32_t v) {
        const auto* b = reinterpret_cast<const std::uint8_t*> (&v);
        out.insert (out.end(), b, b + 4);
    };
    put (3);
    for (const auto v : raw)
        put (v);
    put (transientSend);
    put (sustainSend);
    put (sidechain);
    put (60);
    for (int i = 0; i < 4; ++i)
        out.push_back (1);
    return out;
}

std::array<int, lynx::transmitter::count> defaultRaw()
{
    std::array<int, lynx::transmitter::count> raw {};
    const auto d = lynx::transmitter::defaults();
    for (int k = 0; k < lynx::transmitter::count; ++k)
        raw[(std::size_t) k] = lynx::transmitter::rawFromParam (k, d[(std::size_t) k]);
    return raw;
}

// the measuring input (1 s, 44.1 kHz): noise bursts four times a second over a 220.5 Hz triangle (the goldens' in2.f32)
float noise (unsigned& s)
{
    s = s * 1664525u + 1013904223u;
    return (float) (s >> 8) * (1.0f / 8388608.0f) - 1.0f;
}
void input (std::vector<float>& l, std::vector<float>& r)
{
    l.resize (44100);
    r.resize (44100);
    unsigned s = 1;
    for (int i = 0; i < 44100; ++i)
    {
        const int k = i % 11025;
        const float env = k < 3000 ? (float) (3000 - k) / 3000.0f : 0.0f;
        const float tri = (float) std::abs (i % 200 - 100) / 50.0f - 1.0f;
        const float a = noise (s), b = noise (s);
        l[(std::size_t) i] = 0.8f * env * a + 0.2f * tri;
        r[(std::size_t) i] = 0.8f * env * b + 0.2f * tri;
    }
}

// the effect as a mixer slot runs it: blocks of 512 at 120 BPM
void render (const std::array<int, lynx::transmitter::count>& raw, std::vector<float>& l, std::vector<float>& r)
{
    auto fx = lynx::makeEffect (lynx::EffectKind::transmitter);
    fx->prepare (44100.0);
    lynx::EffectParams p {};
    for (int k = 0; k < lynx::transmitter::count; ++k)
        p[(std::size_t) k] = lynx::transmitter::paramFromRaw (k, raw[(std::size_t) k]);
    input (l, r);
    for (int s = 0; s < 44100; s += 512)
        fx->process (l.data() + s, r.data() + s, std::min (512, 44100 - s), p, 120.0);
}

double rms (const std::vector<float>& x)
{
    double e = 0;
    for (const auto v : x)
        e += (double) v * v;
    return std::sqrt (e / (double) x.size());
}
} // namespace fltrans165

TEST (fltrans165_controls_and_state)
{
    using namespace lynx::transmitter;
    // FL's ranges and mappings (the front end's forward map, REA §1)
    CHECK (controlRange (sensitivity).hi == 10000 && controlRange (mode).hi == 2 && controlRange (sustainEffect).hi == 5
           && controlRange (lookahead).hi == 1);
    CHECK (engineValue (hold, 10000) == 250.0f && engineValue (release, 0) == 1.0f);
    CHECK (engineValue (transientLevel, 0) == -120.0f && engineValue (sustainLevel, 10000) == 12.0f);
    CHECK (std::abs (engineValue (tDiffDelay, 0) - 0.1f) < 1e-6f && std::abs (engineValue (sDiffDelay, 10000) - 10.0f) < 1e-5f);
    CHECK (std::abs (engineValue (tDelayTime, 10000) - 2.0f) < 1e-6f && engineValue (sDelaySynced, 5000) == 8.0f);
    CHECK (std::abs (engineValue (tDiffFeedback, 10000) - 0.99f) < 1e-6f);
    for (int k = 0; k < count; ++k)
        for (const int raw : { controlRange (k).lo, (controlRange (k).lo + controlRange (k).hi) / 2, controlRange (k).hi })
            CHECK (rawFromParam (k, paramFromRaw (k, raw)) == raw);
    // FL's Default preset: Saturation on the transient, Diffusion on the sustain, Balanced, lookahead on
    const auto raw = fltrans165::defaultRaw();
    CHECK (raw[transientEffect] == saturation && raw[sustainEffect] == diffusion && raw[mode] == 1 && raw[lookahead] == 1);
    CHECK (paramText (sustainEffect, paramFromRaw (sustainEffect, diffusion)) == "Diffusion" && paramText (mode, 1.0f) == "Dense");
    CHECK (paramText (hold, 1.0f) == "250.0 ms" && paramText (tDiffDelay, 1.0f) == "10.00 s" && paramText (transientOn, 1.0f) == "On");

    // the state: the raws, the sends and the sidechain input; other versions / short blobs are refused
    auto custom = raw;
    custom[sensitivity] = 1234;
    custom[sustainEffect] = bounce;
    const auto blob = fltrans165::flState (custom, 3, 4, 1);
    CHECK (blob.size() == 192);
    const auto st = parse (blob);
    CHECK (st && st->raw == custom && st->transientSend == 3 && st->sustainSend == 4 && st->sidechain == 1);
    auto wrong = blob;
    wrong[0] = 7;
    CHECK (! parse (wrong) && ! parse ({ 3, 0, 0, 0 }));

    // the import's slot
    const auto fx = lynx::effectFromFl ("Transmitter", blob, 120.0);
    CHECK (fx && fx->kind == lynx::EffectKind::transmitter);
    if (fx)
        for (int k = 0; k < count; ++k)
            CHECK (rawFromParam (k, fx->params[(std::size_t) k]) == custom[(std::size_t) k]);
    CHECK (lynx::effectForFlPlugin ("Transmitter") == lynx::EffectKind::transmitter);
    CHECK (lynx::flPluginsForEffect (lynx::EffectKind::transmitter) == std::vector<std::string> { "Transmitter" });
    CHECK (std::string (lynx::effectInfo (lynx::EffectKind::transmitter).name) == "Transmitter"
           && lynx::effectInfo (lynx::EffectKind::transmitter).params.size() == (std::size_t) count);
}

TEST (fltrans165_matches_fl_engine)
{
    // FL's engine (Shared\ildsp_x64.dll, Transmitter_CreateInstance) run by the EXEC harness on the same input, blocks of
    // 512 at 44.1 kHz, 120 BPM: RMS per channel and samples (REA §4). LYNX's engine nulls with FL's at −126 dB (Saturation:
    // tanh) / −141 dB (Bounce: the tap sum's order) / bit-exact (Delay, Diffusion, the split, levels, tilt).
    using namespace lynx::transmitter;
    struct Golden
    {
        const char* name;
        std::vector<std::pair<int, int>> raw;  // over FL's Default preset
        double rmsL, rmsR, tolerance;
        std::vector<std::tuple<int, float, float>> samples;
    };
    const std::vector<Golden> goldens {
        { "default (Saturation / Diffusion)", {}, 0.5844048, 0.5804336, 1e-4,
          { { 1000, 0.21707967f, 1.01091170f }, { 5000, -1.03529537f, -0.03457152f }, { 12000, -3.15988159f, -1.13557339f },
            { 20000, -0.64697671f, -0.23627697f }, { 30000, -0.15041187f, -0.21635322f }, { 43000, -0.18586665f, -0.31269926f } } },
        { "Bounce / Bounce", { { transientEffect, bounce }, { sustainEffect, bounce } }, 3.1771941, 3.2241741, 1e-4,
          { { 1000, 15.54826736f, 11.71724796f }, { 5000, -0.91758943f, 1.50666320f }, { 12000, 0.27385971f, 0.92732733f },
            { 20000, -0.09705693f, 1.38487291f }, { 30000, -0.53903580f, 0.41527802f }, { 43000, -2.76334548f, 0.35765517f } } },
        { "Delay / Delay, transient tempo-synced", { { transientEffect, delay }, { sustainEffect, delay }, { tDelaySync, 1 } },
          0.3699858, 0.3729715, 1e-6,
          { { 1000, 0.72713578f, -0.63157409f }, { 5000, 0.07070919f, -0.67177099f }, { 12000, 0.36333966f, -1.17254472f },
            { 20000, 0.00034775f, 0.00034780f }, { 30000, -0.17344254f, -0.17344281f }, { 43000, -0.00041140f, -0.00041207f } } },
        { "no lookahead, Dense, Off / Blur", { { lookahead, 0 }, { mode, 2 }, { transientEffect, off }, { sustainEffect, blur },
                                               { sensitivity, 9000 }, { transientLevel, 7000 } },
          0.2640906, 0.2638209, 1e-3,
          { { 1000, 0.16647908f, -0.12186517f }, { 5000, 0.21776281f, 0.21776281f }, { 12000, 0.41254151f, 0.65830296f },
            { 20000, 0.48200831f, 0.48200831f }, { 30000, 0.48200950f, 0.48200950f }, { 43000, 0.48200950f, 0.48200950f } } },
    };
    for (const auto& g : goldens)
    {
        auto raw = fltrans165::defaultRaw();
        for (const auto& [k, v] : g.raw)
            raw[(std::size_t) k] = v;
        std::vector<float> l, r;
        fltrans165::render (raw, l, r);
        bool ok = std::abs (fltrans165::rms (l) - g.rmsL) < g.rmsL * g.tolerance && std::abs (fltrans165::rms (r) - g.rmsR) < g.rmsR * g.tolerance;
        for (const auto& [i, gl, gr] : g.samples)
            ok = ok && std::abs (l[(std::size_t) i] - gl) <= g.tolerance * std::max (1.0f, std::abs (gl))
                 && std::abs (r[(std::size_t) i] - gr) <= g.tolerance * std::max (1.0f, std::abs (gr));
        if (! ok)
            std::printf ("  %s: rms %.7f / %.7f (FL %.7f / %.7f)\n", g.name, fltrans165::rms (l), fltrans165::rms (r), g.rmsL, g.rmsR);
        CHECK (ok);
    }
}

TEST (fltrans165_latency_and_sidechain)
{
    using namespace lynx::transmitter;
    auto fx = lynx::makeEffect (lynx::EffectKind::transmitter);
    fx->prepare (44100.0);
    auto raw = fltrans165::defaultRaw();
    lynx::EffectParams p {};
    auto set = [&] {
        for (int k = 0; k < count; ++k)
            p[(std::size_t) k] = paramFromRaw (k, raw[(std::size_t) k]);
    };
    set();
    std::vector<float> l (512, 0.0f), r (512, 0.0f);
    fx->process (l.data(), r.data(), 512, p, 120.0);
    CHECK (fx->latencySamples() == 88); // 2 ms lookahead
    raw[lookahead] = 0;
    set();
    fx->process (l.data(), r.data(), 512, p, 120.0);
    CHECK (fx->latencySamples() == 0);

    // the transient alone (sustain at −120 dB, no section effects): the input's bursts pass; with a silent sidechain the
    // detector sees nothing, so everything is sustain and the output falls silent
    raw[sustainLevel] = 0;
    raw[transientEffect] = off;
    raw[sustainEffect] = off;
    auto run = [&] (bool side) {
        auto e = lynx::makeEffect (lynx::EffectKind::transmitter);
        e->prepare (44100.0);
        set();
        std::vector<float> a, b;
        fltrans165::input (a, b);
        for (int s = 0; s < 44100; s += 512)
        {
            if (side)
                e->setSidechain (nullptr, nullptr);
            e->process (a.data() + s, b.data() + s, std::min (512, 44100 - s), p, 120.0);
        }
        return fltrans165::rms (a);
    };
    const auto open = run (false), sided = run (true);
    CHECK (open > 0.05 && sided < open * 1e-4);

    // reset (the audio thread's, after a non-finite block): the delay lines and the Diffusion / Bounce taps fall silent
    raw = fltrans165::defaultRaw();
    raw[transientEffect] = bounce;
    raw[sustainEffect] = delay;
    set();
    std::vector<float> a, b;
    fltrans165::input (a, b);
    for (int s = 0; s < 44100; s += 512)
        fx->process (a.data() + s, b.data() + s, std::min (512, 44100 - s), p, 120.0);
    fx->reset();
    std::vector<float> zl (4096, 0.0f), zr (4096, 0.0f);
    for (int s = 0; s < 4096; s += 512)
        fx->process (zl.data() + s, zr.data() + s, 512, p, 120.0);
    CHECK (fltrans165::rms (zl) == 0.0 && fltrans165::rms (zr) == 0.0);
}

TEST (fltrans165_flp_import)
{
    // insert 1 holds a Transmitter whose sidechain is its first input (insert 2 routes into it) and whose transient goes to
    // a send track: the slot follows the sidechain, the sends are reported as not routed
    using namespace lynx::transmitter;
    flp::Project project;
    project.tempo = 120.0;
    flp::MixerInsert one, two;
    one.index = 1;
    two.index = 2;
    two.hasRoutes = true;
    two.routes = { 1 };
    flp::MixerInsert::Slot s;
    s.index = 0;
    s.plugin = "Transmitter";
    auto raw = fltrans165::defaultRaw();
    raw[sustainEffect] = delay;
    s.state = fltrans165::flState (raw, 5, 0, 1);
    one.slots.push_back (s);
    project.mixer = { one, two };
    lynx::FlpImportReport report;
    lynx::FlpImportOptions options;
    options.report = &report;
    const auto song = lynx::importFlp (project, options);
    CHECK (song.mixer.size() > 2 && song.mixer[1].effects.size() == 1);
    if (song.mixer.size() < 3 || song.mixer[1].effects.empty())
        return;
    const auto& fx = song.mixer[1].effects[0];
    CHECK (fx.kind == lynx::EffectKind::transmitter && fx.sidechain == 2 && rawFromParam (sustainEffect, fx.params[sustainEffect]) == delay);
    CHECK (report.partialEffects.count ("Transmitter (transient / sustain sends to other tracks not routed)") == 1);
    CHECK (report.droppedEffects.count ("Transmitter") == 0);
}
