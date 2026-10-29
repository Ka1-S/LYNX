// --- Fruity Scratcher (core/Scratcher.h) - round 165g, sub-round flscratch165 (REA_rodada165g-flscratch165.md)

#include "core/Scratcher.h"

namespace flscr165
{
// FL's state as the installation's render test holds it (version 0xF4628, 14 floats, the sample's path)
std::vector<std::uint8_t> flState (std::uint32_t version, std::initializer_list<float> values, const std::string& path)
{
    std::vector<std::uint8_t> out;
    auto put = [&out] (const void* p, std::size_t n) {
        const auto* b = static_cast<const std::uint8_t*> (p);
        out.insert (out.end(), b, b + n);
    };
    put (&version, 4);
    for (const auto v : values)
        put (&v, 4);
    const auto n = (std::int32_t) path.size();
    put (&n, 4);
    put (path.data(), path.size());
    return out;
}

// a stereo sine of `cycles` periods over `frames` frames, on the 16-bit grid
std::shared_ptr<lynx::SampleData> sine (int frames, int cycles, float amp = 0.5f)
{
    auto s = std::make_shared<lynx::SampleData>();
    s->left.resize ((std::size_t) frames);
    s->right.resize ((std::size_t) frames);
    for (int i = 0; i < frames; ++i)
    {
        const auto v = std::round (amp * 32767.0 * std::sin (2.0 * 3.141592653589793 * cycles * i / frames)) / 32768.0;
        s->left[(std::size_t) i] = s->right[(std::size_t) i] = (float) v;
    }
    s->sampleRate = 44100.0;
    return s;
}

// zero crossings (rising) per second of a block
double risingPerSecond (const std::vector<float>& x, double rate)
{
    int n = 0;
    for (std::size_t i = 1; i < x.size(); ++i)
        n += x[i - 1] < 0.0f && x[i] >= 0.0f;
    return n * rate / (double) x.size();
}

lynx::EffectParams params (std::initializer_list<std::pair<int, float>> set)
{
    auto p = lynx::makeEffectSlot (lynx::EffectKind::scratcher).params;
    for (const auto& [k, v] : set)
        p[(std::size_t) k] = v;
    return p;
}

// runs the effect for `blocks` blocks of 256 on a silent input; returns the last `keep` samples (left, right)
std::pair<std::vector<float>, std::vector<float>> run (lynx::Effect& e, const lynx::EffectParams& p, int blocks, int keep)
{
    std::vector<float> l, r;
    for (int b = 0; b < blocks; ++b)
    {
        std::vector<float> bl (256, 0.0f), br (256, 0.0f);
        e.process (bl.data(), br.data(), 256, p, 120.0);
        l.insert (l.end(), bl.begin(), bl.end());
        r.insert (r.end(), br.begin(), br.end());
    }
    const auto from = l.size() - (std::size_t) std::min<std::size_t> ((std::size_t) keep, l.size());
    return { { l.begin() + (long) from, l.end() }, { r.begin() + (long) from, r.end() } };
}

double rms (const std::vector<float>& x)
{
    double s = 0.0;
    for (const auto v : x)
        s += (double) v * v;
    return x.empty() ? 0.0 : std::sqrt (s / (double) x.size());
}
} // namespace flscr165

TEST (flscr165_controls_and_state)
{
    using namespace lynx::scratcher;
    // FL's control ranges (the editor's DFM) and their steps
    CHECK (controlRange (wavePos).hi == 30000 && controlRange (pan).lo == -128 && controlRange (volume).hi == 255);
    CHECK (rawOfValue (volume, 0.78f) == 199 && rawOfValue (wavePos, 0.111f) == 3330 && rawOfValue (pan, 0.5f) == 0);
    CHECK (valueOfRaw (wavePos, 30000) == 1.0f && valueOfRaw (transport, -1) == 0.0f && valueOfRaw (transport, 1) == 1.0f);
    CHECK (transportOf (0.0f) == -1 && transportOf (0.5f) == 0 && transportOf (1.0f) == 1 && rotationsOf (0.5f) == 9);
    CHECK (quantize (speed, 0.504f) == valueOfRaw (speed, 50));
    const auto d = defaults();
    CHECK (rawOfValue (wavePos, d[wavePos]) == 690 && rawOfValue (volume, d[volume]) == 199 && transportOf (d[transport]) == 0);
    CHECK (paramText (transport, 1.0f) == "Forwards" && paramText (speed, 0.5f) == "1.00x" && paramText (rotations, 0.5f) == "+0");

    // the render test's state: Wave position 0.111, sound on (1), Hold on, transport stopped, Rotations 9
    const auto blob = flscr165::flState (0xF4628u, { 0.111f, 1.0f, 0.5f, 0.25f, 0.5f, 0.78f, 1.0f, 0.5f, 0.0f, 0.5f, 0.908f, 0.999f, 0.4f, 9.0f },
                                         "%FLStudioFactoryData%\\Data\\Patches\\Packs\\Loops\\DL Breaker.wav");
    const auto st = parse (blob);
    CHECK (st.has_value());
    if (st)
    {
        CHECK (st->values[wavePos] == valueOfRaw (wavePos, 3330) && st->values[mute] == 0.0f && st->values[hold] == 1.0f);
        CHECK (st->values[volume] == valueOfRaw (volume, 199) && transportOf (st->values[transport]) == 0 && rotationsOf (st->values[rotations]) == 9);
        CHECK (st->samplePath == "%FLStudioFactoryData%\\Data\\Patches\\Packs\\Loops\\DL Breaker.wav" && st->timecode == -1);
        // LYNX writes FL 2026's version; reading it back gives the same values
        const auto again = parse (toFl (st->values, st->samplePath));
        CHECK (again && again->values == st->values && again->samplePath == st->samplePath && again->version == 0xF462Au);
    }
    // FL's sound flag 0 = muted; unknown versions are refused
    const auto muted = parse (flscr165::flState (0xF4628u, { 0, 0, 0.5f, 0.25f, 0.5f, 0.78f, 0, 0.5f, 1, 0.5f, 0.908f, 0.999f, 0.4f, 9 }, ""));
    CHECK (muted && muted->values[mute] == 1.0f && transportOf (muted->values[transport]) == 1 && muted->samplePath.empty());
    CHECK (! parse (flscr165::flState (0xF4627u, {}, "")) && ! parse ({ 1, 2, 3 }));

    // the import's slot: kind, parameters
    const auto fx = lynx::effectFromFl ("Fruity Scratcher", blob, 140.0);
    CHECK (fx && fx->kind == lynx::EffectKind::scratcher && st && fx->params[wavePos] == st->values[wavePos] && fx->params[hold] == 1.0f);
    CHECK (lynx::effectForFlPlugin ("Fruity Scratcher") == lynx::EffectKind::scratcher);
}

TEST (flscr165_sample_as_fl_hosts_it)
{
    using namespace lynx::scratcher;
    // 16-bit values stay; others are rounded as round(x · 32767) / 32768; the extra frame is frame 0's right side
    const float l[3] = { 0.5f, -0.25f, 0.1f }, r[3] = { 0.125f, 0.0f, 0.3f };
    const auto t = prepareSample (l, r, 3, 44100.0);
    CHECK (t && t->n == 3 && t->frames.size() == 8);
    if (t)
    {
        CHECK (t->frames[0] == (float) (std::lround (0.5 * 32767) / 32768.0) && t->frames[1] == (float) (std::lround (0.125 * 32767) / 32768.0));
        CHECK (t->frames[6] == t->frames[1] && t->frames[7] == t->frames[1]);
    }
    const float g[2] = { 16384.0f / 32768.0f, -3.0f / 32768.0f };
    const auto grid = prepareSample (g, nullptr, 2, 44100.0);
    CHECK (grid && grid->frames[0] == g[0] && grid->frames[1] == g[0] && grid->frames[2] == g[1] && grid->frames[4] == g[0]);
    // another rate: resampled to 44.1 kHz
    std::vector<float> long48 (48000, 0.0f);
    const auto r48 = prepareSample (long48.data(), nullptr, long48.size(), 48000.0);
    CHECK (r48 && r48->n == 44100);
}

TEST (flscr165_motor_mode)
{
    using namespace lynx::scratcher;
    // a 441 Hz sine (100 cycles over 0.1 s · 10): Speed 0.5 = 1×, Speed 1 = 2× (2 · Speed / (n − 1) turns per sample)
    const auto smp = flscr165::sine (44100, 441);
    for (const auto& [spd, want] : std::vector<std::pair<float, double>> { { 0.5f, 441.0 }, { 1.0f, 882.0 }, { 0.25f, 220.5 } })
    {
        auto e = make (smp);
        e->prepare (44100.0);
        const auto p = flscr165::params ({ { transport, 1.0f }, { speed, spd }, { accel, 1.0f }, { volume, 1.0f } });
        const auto [l, r] = flscr165::run (*e, p, 600, 44100); // the inertia (≈ 2000 samples at Acceleration 1) has settled
        const auto hz = flscr165::risingPerSecond (l, 44100.0);
        CHECK (std::abs (hz - want) < 2.0);
        if (std::abs (hz - want) >= 2.0)
            std::printf ("  motor speed %.2f: %.2f Hz (want %.2f)\n", spd, hz, want);
        CHECK (flscr165::rms (l) > 0.3 && std::abs (flscr165::rms (l) - flscr165::rms (r)) < 1e-6); // centred
    }
    // backwards: the position runs down; stopped: silence once the DC blocker has settled
    {
        auto e = make (smp);
        const auto p = flscr165::params ({ { transport, 0.0f }, { accel, 1.0f } });
        flscr165::run (*e, p, 40, 16);
        const auto a = e->controlOutput (0);
        flscr165::run (*e, p, 4, 16);
        const auto b = e->controlOutput (0);
        CHECK (a != b && (b < a || a < 0.05f));
        auto s = make (smp);
        const auto [l, r] = flscr165::run (*s, flscr165::params ({}), 400, 4096);
        CHECK (flscr165::rms (l) < 1e-6);
    }
    // the input passes with gain 1; without a sample nothing is added
    {
        auto e = make (nullptr);
        std::vector<float> l (64, 0.25f), r (64, -0.5f);
        e->process (l.data(), r.data(), 64, flscr165::params ({ { transport, 1.0f } }), 120.0);
        CHECK (l[10] == 0.25f && r[63] == -0.5f);
    }
    // like FL, the sample takes over at the end of the first block: that block adds nothing
    {
        auto e = make (smp);
        std::vector<float> l (256, 0.0f), r (256, 0.0f);
        e->process (l.data(), r.data(), 256, flscr165::params ({ { transport, 1.0f }, { accel, 1.0f } }), 120.0);
        CHECK (flscr165::rms (l) == 0.0);
    }
}

TEST (flscr165_hold_mute_pan)
{
    using namespace lynx::scratcher;
    const auto smp = flscr165::sine (44100, 441);
    // Hold: a still platter is silent (DC blocker); moving Wave position plays the sample
    {
        auto e = make (smp);
        const auto still = flscr165::params ({ { hold, 1.0f }, { wavePos, 0.3f }, { volume, 1.0f } });
        const auto [l, r] = flscr165::run (*e, still, 2000, 4096); // the smoother converges within ~2 s
        CHECK (flscr165::rms (l) < 1e-3);
        double moving = 0.0;
        for (int b = 0; b < 400; ++b)
        {
            auto p = still;
            p[wavePos] = 0.3f + 0.2f * (float) b / 400.0f;
            const auto [ml, mr] = flscr165::run (*e, p, 1, 256);
            moving = std::max (moving, flscr165::rms (ml));
        }
        CHECK (moving > 0.05);
    }
    // Mute: the gain glides to 0; Pan fully right: the left side is silent (linear balance)
    {
        auto e = make (smp);
        const auto [l, r] = flscr165::run (*e, flscr165::params ({ { transport, 1.0f }, { accel, 1.0f }, { mute, 1.0f } }), 300, 4096);
        CHECK (flscr165::rms (l) < 1e-6 && flscr165::rms (r) < 1e-6);
        auto p = make (smp);
        const auto [pl, pr] = flscr165::run (*p, flscr165::params ({ { transport, 1.0f }, { accel, 1.0f }, { pan, 1.0f } }), 300, 4096);
        CHECK (flscr165::rms (pl) < 1e-6 && flscr165::rms (pr) > 0.2);
    }
}

TEST (flscr165_render_test_import)
{
    // FL's own render test (Data/System/Render tests/Fruity Scratcher.flp): insert 1 slot 0, Hold on, the platter moved
    // by 259 pattern events on Wave position (raw 0…131 of the wheel's 0…30000), DL Breaker.wav from the factory data
    namespace fs = std::filesystem;
    if (realFlpDir.empty())
        return;
    const auto path = fs::path (realFlpDir) / "System" / "Render tests" / "Fruity Scratcher.flp";
    std::ifstream in (path, std::ios::binary);
    const std::vector<std::uint8_t> file ((std::istreambuf_iterator<char> (in)), {});
    if (file.empty())
        return;
    const auto local = [] (std::string p) {
        std::replace (p.begin(), p.end(), '\\', '/');
        return lynx::existingPathIgnoringCase (p);
    };
    lynx::FlpImportReport report;
    lynx::FlpImportOptions options;
    options.report = &report;
    options.roots.data = fs::path (realFlpDir).string();
    options.roots.factoryData = fs::path (realFlpDir).parent_path().string();
    options.fileExists = [local] (const std::string& p) { return fs::is_regular_file (local (p)); };
    const auto song = lynx::importFlp (flp::parseProject (file), options);
    CHECK (song.mixer.size() > 1 && ! song.mixer[1].effects.empty());
    if (song.mixer.size() < 2 || song.mixer[1].effects.empty())
        return;
    const auto& fx = song.mixer[1].effects[0];
    using namespace lynx::scratcher;
    // the state holds Wave position 3330, but the song's start values (0xD8) set it to 0, as FL does when the song starts
    CHECK (fx.kind == lynx::EffectKind::scratcher && fx.params[hold] == 1.0f && rawOfValue (wavePos, fx.params[wavePos]) == 0);
    CHECK (fx.samplePath.find ("DL Breaker.wav") != std::string::npos);
    CHECK (report.converted["Fruity Scratcher"] == 1 && report.droppedEffects.count ("Fruity Scratcher") == 0);
    // the platter's events: a lane on parameter 0 in the wheel's scale (the highest, 131, is 131 / 30000)
    const lynx::PatternEvents* lane = nullptr;
    for (const auto& p : song.patterns)
        for (const auto& l : p.events)
            if (l.target.kind == lynx::AutomationTargetKind::effectParam && l.target.index == 1 && l.target.slot == 0 && l.target.param == 0)
                lane = &l;
    CHECK (lane != nullptr);
    if (lane != nullptr)
    {
        float top = 0.0f;
        for (const auto& e : lane->events)
            top = std::max (top, e.value);
        CHECK (lane->events.size() > 200 && std::abs (top - 131.0f / 30000.0f) < 1e-6f);
    }
}

TEST (flscr165_editor_takes_sample_drops)
{
    // the editor loads a dropped sample (OS drop or LYNX's Browser) instead of letting it fall to the Playlist: the
    // same list as SampleLibrary::isSampleFile (core/SampleFiles.h), FL's .wv packs included, any case
    using lynx::scratcher::loadsDroppedFile;
    for (const char* ok : { "C:\\Samples\\DL Breaker.wav", "/home/u/loop.WAV", "kick.aiff", "pad.flac", "x.ogg", "y.mp3", "C:\\FL\\Packs\\a.wv", "b.aif" })
        CHECK (loadsDroppedFile (ok));
    for (const char* no : { "song.flp", "beat.mid", "C:\\dir.wav\\readme", "noext", "trailing.", "preset.fst", "" })
        CHECK (! loadsDroppedFile (no));
}
