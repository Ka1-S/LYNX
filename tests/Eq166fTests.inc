// Round 166f (issue #16): Facet EQ's graph targets, Facet's comparative EQ, Parametric EQ 2's spectrum colours and the
// plugin folders of the slot menus.

#include "core/FacetEq.h"

TEST (eq166f_facet_press_on_a_node_moves_the_band_not_its_range)
{
    using namespace lynx::facet;
    // band 0 static at (100, 100) — its "range handle" would sit on the node; band 1 dynamic at (300, 100), handle at (300, 160)
    const float nx[] = { 100.0f, 300.0f }, ny[] = { 100.0f, 100.0f };
    const float rx[] = { 100.0f, 300.0f }, ry[] = { 100.0f, 160.0f };
    const bool on[] = { true, true }, dyn[] = { false, true };
    // the reporter's case: the selected band, a press a few pixels off its centre → the node
    for (const auto [x, y] : { std::pair { 100.0f, 100.0f }, { 103.0f, 98.0f }, { 108.0f, 104.0f } })
    {
        const auto h = pickGraphTarget (nx, ny, on, dyn, rx, ry, 2, 0, x, y);
        CHECK (h.band == 0 && ! h.range);
    }
    // a static band has no range target at all
    CHECK (pickGraphTarget (nx, ny, on, dyn, rx, ry, 2, 0, 100.0f, 113.0f).band == -1);
    // a dynamic band: its node still wins near the centre, the handle is reachable on its own
    CHECK (pickGraphTarget (nx, ny, on, dyn, rx, ry, 2, 1, 302.0f, 104.0f).band == 1 && ! pickGraphTarget (nx, ny, on, dyn, rx, ry, 2, 1, 302.0f, 104.0f).range);
    const auto r = pickGraphTarget (nx, ny, on, dyn, rx, ry, 2, 1, 303.0f, 158.0f);
    CHECK (r.band == 1 && r.range);
    CHECK (pickGraphTarget (nx, ny, on, dyn, rx, ry, 2, 1, 309.0f, 160.0f).band == -1); // the handle is small (6 px)
    // a handle pulled close to its node: the node keeps its whole radius
    const float ry2[] = { 100.0f, 108.0f };
    const auto close = pickGraphTarget (nx, ny, on, dyn, rx, ry2, 2, 1, 300.0f, 106.0f);
    CHECK (close.band == 1 && ! close.range);
    const auto below = pickGraphTarget (nx, ny, on, dyn, rx, ry2, 2, 1, 300.0f, 112.5f);
    CHECK (below.band == 1 && below.range);
    // bands switched off are not targets
    const bool off[] = { false, true };
    CHECK (pickGraphTarget (nx, ny, off, dyn, rx, ry, 2, 0, 100.0f, 100.0f).band == -1);
}

#include "core/PluginCatalog.h"

TEST (eq166f_plugin_folders_model_round_trip_and_menu_groups)
{
    namespace pc = lynx::plugincatalog;
    CHECK (pc::cleanFolder ("  Stock // EQ / ") == "Stock/EQ");
    CHECK (pc::cleanFolder ("a/b/c/d/e/f") == "a/b/c/d");
    CHECK (pc::cleanFolder (" / \t ") == "");
    CHECK (pc::parentFolder ("Stock/EQ") == "Stock" && pc::parentFolder ("Stock") == "" && pc::folderLeaf ("Stock/EQ") == "EQ");

    pc::Folders f;
    CHECK (pc::addFolder (f, false, "Stock/EQ") == "Stock/EQ");
    CHECK (f.effects.count ("Stock") == 1 && f.effects.count ("Stock/EQ") == 1 && f.generators.empty());
    pc::moveToFolder (f, false, "lynx:fx:Fruity Parametric EQ 2", "Stock/EQ");
    pc::moveToFolder (f, false, "lynx:fx:Fruity Limiter", "Stock");
    pc::moveToFolder (f, false, "VST3-Pro-Q 4-abc", "Third party/EQ");
    pc::moveToFolder (f, false, "VST3-Valhalla-xyz", "Third party/Reverb");
    pc::addFolder (f, false, "Empty one");
    pc::moveToFolder (f, true, "lynx:gen:Sytrus", "Synths");

    // saved and read back the same (keys may hold spaces; a key with a tab can't be written)
    const auto text = pc::formatFolders (f);
    const auto g = pc::parseFolders (text);
    CHECK (g.of == f.of && g.effects == f.effects && g.generators == f.generators);
    CHECK (pc::parseFolders ("junk\nkey\tonly\nfx\t\n").of.empty());

    // the menus' folder tree (FL's Categories / Tree views): empty folders and the other kind left out, natural A-Z
    f.view = (int) pc::MenuView::tree;
    CHECK (pc::parseFolders (pc::formatFolders (f)).view == 2);
    const std::vector<std::string> keys { "VST3-Valhalla-xyz", "lynx:fx:Fruity Limiter", "lynx:fx:Fruity Parametric EQ 2", "lynx:fx:Nothing",
                                          "VST3-Pro-Q 4-abc", "lynx:gen:Sytrus" };
    const auto tree = pc::menuTree (f, false, keys);
    CHECK (tree.size() == 2);
    if (tree.size() == 2)
    {
        CHECK (tree[0].path == "Stock" && tree[0].items == std::vector<int> { 1 } && tree[0].sub.size() == 1);
        CHECK (tree[0].sub.size() == 1 && tree[0].sub[0].name == "EQ" && tree[0].sub[0].path == "Stock/EQ" && tree[0].sub[0].items == std::vector<int> { 2 });
        CHECK (tree[1].name == "Third party" && tree[1].items.empty() && tree[1].sub.size() == 2);
        CHECK (tree[1].sub.size() == 2 && tree[1].sub[0].name == "EQ" && tree[1].sub[0].items == std::vector<int> { 4 } && tree[1].sub[1].name == "Reverb");
    }
    CHECK ((pc::allItems (tree) == std::vector<int> { 2, 1, 4, 0 }));
    CHECK (pc::groupHeading ("Stock/EQ") == "Stock \xe2\x80\xba EQ");
    const auto gens = pc::menuTree (f, true, keys);
    CHECK (gens.size() == 1 && gens[0].path == "Synths" && gens[0].items == std::vector<int> { 5 });

    // natural order: "Bus 2" before "Bus 10"
    pc::Folders n;
    pc::moveToFolder (n, false, "a", "Bus 10");
    pc::moveToFolder (n, false, "b", "Bus 2");
    const auto ng = pc::menuTree (n, false, { "a", "b" });
    CHECK (ng.size() == 2 && ng[0].path == "Bus 2" && ng[1].path == "Bus 10");

    // rename moves the subfolders and their plugins; a taken name or a move into itself is refused
    CHECK (pc::renameFolder (f, false, "Third party", "3rd party"));
    CHECK (f.effects.count ("3rd party/EQ") == 1 && f.effects.count ("Third party") == 0 && f.of["VST3-Pro-Q 4-abc"] == "3rd party/EQ");
    CHECK (! pc::renameFolder (f, false, "Stock", "3rd party"));
    CHECK (! pc::renameFolder (f, false, "Stock", "Stock/Inner"));
    // remove: the folder and its subfolders go, their plugins go back to no folder
    pc::removeFolder (f, false, "Stock", keys);
    CHECK (f.effects.count ("Stock") == 0 && f.effects.count ("Stock/EQ") == 0 && f.of.count ("lynx:fx:Fruity Limiter") == 0
           && f.of.count ("lynx:fx:Fruity Parametric EQ 2") == 0 && f.of.count ("VST3-Pro-Q 4-abc") == 1);
    pc::moveToFolder (f, false, "VST3-Pro-Q 4-abc", "");
    CHECK (f.of.count ("VST3-Pro-Q 4-abc") == 0);
}

TEST (eq166f_plugin_folders_from_fl_plugin_database)
{
    namespace pc = lynx::plugincatalog;
    pc::Folders f;
    const std::vector<std::string> files { "Stock/EQ/Fruity Parametric EQ 2.fst", "Stock/Dynamics/Fruity limiter.fst", "Delay reverb/LuxeVerb.fst",
                                           "Top level.fst", "Third party/Unknown Plugin.fst" };
    const std::vector<std::pair<std::string, std::string>> names { { "lynx:fx:Fruity Parametric EQ 2", "Fruity Parametric EQ 2" },
                                                                   { "lynx:fx:Fruity Limiter", "Fruity Limiter" },
                                                                   { "lynx:fx:LuxeVerb", "LuxeVerb" }, { "lynx:fx:Top level", "Top level" } };
    CHECK (pc::importFlFolders (f, false, files, names) == 3);
    CHECK (f.of["lynx:fx:Fruity Parametric EQ 2"] == "Stock/EQ" && f.of["lynx:fx:Fruity Limiter"] == "Stock/Dynamics" && f.of["lynx:fx:LuxeVerb"] == "Delay reverb");
    CHECK (f.of.count ("lynx:fx:Top level") == 0 && f.effects.count ("Third party") == 0);
}

namespace eq166f
{
// runs `secs` of a source tone (fs Hz, as) and a sidechain tone (ft Hz, at) through Facet in 480-sample blocks;
// returns the output's left channel and the input / sidechain copies
struct Run { std::vector<float> out, src, tgt; int latency = 0; };
Run run (const lynx::EffectParams& p, double secs, double fs, double as, double ft, double at, bool side = true, int phaseMode = 0)
{
    using namespace lynx::facet;
    auto params = p;
    params[(std::size_t) phaseParam] = phaseValue ((Phase) phaseMode);
    auto e = make (&params);
    e->prepare (48000.0);
    const int n = (int) (secs * 48000.0);
    Run r;
    std::vector<float> l ((std::size_t) n), rr ((std::size_t) n), sl ((std::size_t) n), sr ((std::size_t) n);
    for (int i = 0; i < n; ++i)
    {
        l[(std::size_t) i] = rr[(std::size_t) i] = (float) (as * std::sin (2.0 * 3.14159265358979 * fs * i / 48000.0));
        sl[(std::size_t) i] = sr[(std::size_t) i] = (float) (at * std::sin (2.0 * 3.14159265358979 * ft * i / 48000.0));
    }
    r.src = l;
    r.tgt = sl;
    for (int o = 0; o < n; o += 480)
    {
        const int m = std::min (480, n - o);
        if (side) e->setSidechain (sl.data() + o, sr.data() + o);
        e->process (l.data() + o, rr.data() + o, m, params, 120.0);
    }
    r.out = l;
    r.latency = e->latencySamples();
    return r;
}
double rms (const std::vector<float>& v, int from, int to)
{
    double s = 0.0;
    for (int i = from; i < to; ++i) s += (double) v[(std::size_t) i] * v[(std::size_t) i];
    return std::sqrt (s / std::max (1, to - from));
}
} // namespace eq166f

TEST (eq166f_facet_comparative_eq_listens_to_source_and_target)
{
    using namespace lynx::facet;
    auto p = facet103::bell (1000.0, 12.0, 1.0); // a loud boost: the EQ's output is clearly not the source
    CHECK (listen (p[(std::size_t) listenParam]) == Listen::eq && p[(std::size_t) targetLevelParam] >= 0.5f);
    CHECK (std::string (lynx::formatEffectParam (lynx::EffectKind::facetEq, listenParam, listenValue (Listen::target), 120.0)) == "Target");
    const int n = 48000;
    for (const int mode : { 0, 2 }) // zero latency and linear phase (the switch keeps the latency)
    {
        // the EQ (default): +12 dB at 1 kHz
        const auto eq = eq166f::run (p, 1.0, 1000.0, 0.1, 300.0, 0.2, true, mode);
        CHECK (std::abs (20.0 * std::log10 (eq166f::rms (eq.out, n / 2, n) / eq166f::rms (eq.src, n / 2, n)) - 12.0) < 0.5);
        // Source: the input itself, delayed by the latency
        p[(std::size_t) listenParam] = listenValue (Listen::source);
        const auto src = eq166f::run (p, 1.0, 1000.0, 0.1, 300.0, 0.2, true, mode);
        double err = 0.0;
        for (int i = n / 2; i < n; ++i) err = std::max (err, (double) std::abs (src.out[(std::size_t) i] - src.src[(std::size_t) (i - src.latency)]));
        CHECK (err < 1e-6);
        // Target, as is: the sidechain, delayed by the latency
        p[(std::size_t) listenParam] = listenValue (Listen::target);
        p[(std::size_t) targetLevelParam] = 0.0f;
        const auto tgt = eq166f::run (p, 1.0, 1000.0, 0.1, 300.0, 0.2, true, mode);
        err = 0.0;
        for (int i = n / 2; i < n; ++i) err = std::max (err, (double) std::abs (tgt.out[(std::size_t) i] - tgt.tgt[(std::size_t) (i - tgt.latency)]));
        CHECK (err < 1e-6);
        // Target, level-matched: the sidechain at the source's level (0.2 → 0.1: -6 dB)
        p[(std::size_t) targetLevelParam] = 1.0f;
        const auto matched = eq166f::run (p, 6.0, 1000.0, 0.1, 300.0, 0.2, true, mode);
        const int m = 6 * 48000;
        CHECK (std::abs (20.0 * std::log10 (eq166f::rms (matched.out, m - 48000, m) / 0.0707107)) < 0.3);
        // no sidechain routed: the target is silence
        const auto none = eq166f::run (p, 1.0, 1000.0, 0.1, 300.0, 0.2, false, mode);
        CHECK (eq166f::rms (none.out, n / 2, n) < 1e-6);
        p[(std::size_t) listenParam] = listenValue (Listen::eq);
    }
    CHECK (targetMatchGain (0.0, 1.0) == 1.0 && targetMatchGain (1.0, 0.0) == 1.0);
    CHECK (std::abs (targetMatchGain (1.0, 4.0) - 0.5) < 1e-12 && std::abs (targetMatchGain (1e6, 1e-6) - 15.8489) < 1e-3);
}

TEST (eq166f_facet_listen_switch_is_click_free)
{
    using namespace lynx::facet;
    // the same signal on both sides at the same level: switching EQ ↔ target mid-stream must not jump
    auto p = facet103::defaults(); // a flat EQ: output == input
    p[(std::size_t) targetLevelParam] = 0.0f;
    auto e = make (&p);
    e->prepare (48000.0);
    const int n = 48000;
    std::vector<float> l ((std::size_t) n), r ((std::size_t) n), s ((std::size_t) n);
    for (int i = 0; i < n; ++i) l[(std::size_t) i] = r[(std::size_t) i] = s[(std::size_t) i] = (float) (0.5 * std::sin (2.0 * 3.14159265358979 * 220.0 * i / 48000.0));
    const auto ref = l;
    float worst = 0.0f;
    for (int o = 0, k = 0; o < n; o += 480, ++k)
    {
        p[(std::size_t) listenParam] = listenValue (k % 10 < 5 ? Listen::eq : (k % 20 < 10 ? Listen::target : Listen::source));
        e->setSidechain (s.data() + o, s.data() + o);
        e->process (l.data() + o, r.data() + o, 480, p, 120.0);
    }
    for (int i = 0; i < n; ++i) worst = std::max (worst, std::abs (l[(std::size_t) i] - ref[(std::size_t) i]));
    CHECK (worst < 1e-5f);
}

#include "core/SpectrumHeat.h"

TEST (eq166f_peq2_heatmap_colours_follow_the_level)
{
    namespace sh = lynx::spectrumheat;
    CHECK (sh::heat01 (-96.0) == 0.0 && sh::heat01 (0.0) == 1.0 && std::abs (sh::heat01 (-48.0) - 0.5) < 1e-12 && sh::heat01 (-200.0) == 0.0);
    // hotter = brighter and redder: the ramp's brightness and its red rise with the level
    int lastSum = -1, lastRed = -1;
    for (double h = 0.0; h <= 1.0001; h += 0.05)
    {
        const auto c = sh::colour (h);
        const int sum = c.r + c.g + c.b;
        CHECK (sum >= lastSum && (int) c.r >= lastRed);
        lastSum = sum;
        lastRed = c.r;
    }
    // cold end is blue-violet (more blue than red), hot end orange-gold (red > green > blue)
    CHECK (sh::colour (0.3).b > sh::colour (0.3).r);
    CHECK (sh::colour (0.95).r > sh::colour (0.95).g && sh::colour (0.95).g > sh::colour (0.95).b);
    // a band's boost / cut moves its region's heat: +6 dB → hotter, −6 dB → colder (the heat shows the EQ's output)
    sh::Average flat, boosted, cut;
    for (int i = 0; i < 300; ++i) { flat.add (-30.0, 30.0, 1.0); boosted.add (-24.0, 30.0, 1.0); cut.add (-36.0, 30.0, 1.0); }
    CHECK (std::abs (flat.db() + 30.0) < 0.01 && std::abs (boosted.db() + 24.0) < 0.01);
    CHECK (sh::heat01 (boosted.db()) > sh::heat01 (flat.db()) && sh::heat01 (cut.db()) < sh::heat01 (flat.db()));
    // the colouring is relative to the loudest column, so a +12 dB band shows clearly whatever the song's level
    CHECK (std::abs (sh::relative01 (-40.0, -20.0) - (1.0 - 20.0 / 48.0)) < 1e-12 && sh::relative01 (-20.0, -20.0) == 1.0);
    CHECK (sh::relative01 (-200.0, -20.0) == 0.0 && sh::relative01 (-90.0, -20.0) == 0.0);
    CHECK (std::abs (sh::relative01 (-28.0, -10.0) - sh::relative01 (-40.0, -10.0) - 12.0 / 48.0) < 1e-12);
    // the tilt evens out music's slope: 0 at the pivot, +4.5 dB an octave above, -4.5 dB an octave below
    CHECK (sh::tilt (1000.0) == 0.0 && std::abs (sh::tilt (2000.0) - 4.5) < 1e-12 && std::abs (sh::tilt (500.0) + 4.5) < 1e-12);
    // the average follows slowly (about a second): one loud frame doesn't light the column
    sh::Average a;
    for (int i = 0; i < 60; ++i) a.add (-60.0, 30.0, 1.0);
    a.add (0.0, 30.0, 1.0);
    CHECK (a.db() < -14.0);
    // position: Full everywhere; Top / Bottom fade out before the middle
    CHECK (sh::strength (sh::Position::full, 0.9) == 1.0 && sh::strength (sh::Position::top, 0.0) == 1.0 && sh::strength (sh::Position::top, 0.7) == 0.0);
    CHECK (sh::strength (sh::Position::bottom, 1.0) == 1.0 && sh::strength (sh::Position::bottom, 0.2) == 0.0 && sh::strength (sh::Position::top, 0.3) == 0.5);
}

// Round 166f: the selftest's effect fuzz found Echoform giving non-finite output (its random values moved when Facet EQ
// got 2 parameters): the tape voicings' loop (G·F(z)) had no bound, so feedback at the top with a short time grew until
// it overflowed. Every parameter at 0 / 1 / random, three rates, noise in: the output stays finite.
TEST (eq166f_echoform_stays_finite_with_extreme_parameters)
{
    using namespace lynx;
    const auto& info = effectInfo (EffectKind::echoform);
    std::mt19937 rng (166);
    std::uniform_real_distribution<float> u (0.0f, 1.0f);
    std::uniform_int_distribution<int> pick (0, 5);
    int broken = 0;
    for (int set = 0; set < 300 && broken < 3; ++set)
    {
        auto p = makeEffectSlot (EffectKind::echoform).params;
        for (std::size_t i = 0; i < info.params.size(); ++i)
        {
            const auto r = pick (rng);
            p[i] = r == 0 ? 0.0f : r == 1 ? 1.0f : u (rng);
        }
        for (const auto rate : { 44100.0, 48000.0, 96000.0 })
        {
            auto fx = makeEffect (EffectKind::echoform);
            fx->prepare (rate);
            std::vector<float> l (512), r (512);
            bool finite = true;
            for (int block = 0; block < 40 && finite; ++block)
            {
                for (int i = 0; i < 512; ++i) { l[(std::size_t) i] = u (rng) * 2.0f - 1.0f; r[(std::size_t) i] = u (rng) * 2.0f - 1.0f; }
                fx->process (l.data(), r.data(), 512, p, 120.0);
                for (int i = 0; i < 512; ++i)
                    finite = finite && std::isfinite (l[(std::size_t) i]) && std::isfinite (r[(std::size_t) i]);
            }
            if (! finite)
            {
                ++broken;
                std::printf ("  echoform non-finite at %.0f Hz with:", rate);
                for (std::size_t i = 0; i < info.params.size(); ++i) std::printf (" %zu=%.4f", i, (double) p[i]);
                std::printf ("\n");
                break;
            }
        }
    }
    CHECK (broken == 0);
}
