// --- Visual plugins: Wave Candy and the Visualizer (core/VisualFx.h) - round 164c -----------------------------------

#include "core/VisualFx.h"

namespace visual164
{
std::vector<float> sine (double hz, double rate, int n, float amplitude = 1.0f)
{
    std::vector<float> x ((std::size_t) n);
    for (int i = 0; i < n; ++i)
        x[(std::size_t) i] = amplitude * (float) std::sin (2.0 * 3.14159265358979 * hz * i / rate);
    return x;
}
} // namespace visual164

TEST (visual164_tap_ring_and_pass_through)
{
    using namespace lynx;
    auto fx = visualfx::make();
    fx->prepare (48000.0);
    auto* tap = visualfx::tapOf (fx.get());
    CHECK (tap != nullptr && visualfx::tapOf (makeEffect (EffectKind::eq).get()) == nullptr);
    std::vector<float> l (100), r (100);
    for (int i = 0; i < 100; ++i)
    {
        l[(std::size_t) i] = (float) i;
        r[(std::size_t) i] = (float) -i;
    }
    const auto before = l;
    auto slot = makeEffectSlot (EffectKind::waveCandy);
    fx->process (l.data(), r.data(), 100, slot.params, 120.0);
    CHECK (l == before); // the sound is not touched
    std::vector<float> ol (150), orr (150);
    CHECK (tap->read (ol.data(), orr.data(), 50) == 100);
    CHECK (ol[0] == 50.0f && ol[49] == 99.0f && orr[49] == -99.0f);
    // more than was written: zeros first
    tap->read (ol.data(), orr.data(), 150);
    CHECK (ol[0] == 0.0f && ol[49] == 0.0f && ol[50] == 0.0f && ol[51] == 1.0f && ol[149] == 99.0f);
    // the parameters the audio thread used
    CHECK (tap->param (visualfx::wavecandy::mode) == slot.params[0]);
    // the scene lives in the state: an edit keeps the running effect (no rebuild, no click)
    CHECK (fx->acceptState ({ 1, 2, 3 }));
    // wraps around the ring
    std::vector<float> big ((std::size_t) visualfx::tapSize + 10, 0.25f);
    fx->process (big.data(), big.data(), (int) big.size(), slot.params, 120.0);
    tap->read (ol.data(), orr.data(), 10);
    CHECK (ol[9] == 0.25f && tap->written() == 100 + big.size());
}

TEST (visual164_capture_records_only_while_playing)
{
    using namespace lynx;
    auto fx = visualfx::make();
    fx->prepare (44100.0);
    visualfx::Capture cap;
    cap.reserve (256);
    CHECK (visualfx::setCapture (fx.get(), &cap));
    auto slot = makeEffectSlot (EffectKind::visualizer);
    slot.params[0] = 0.75f;
    std::vector<float> l (64, 0.5f), r (64, -0.5f);
    fx->process (l.data(), r.data(), 64, slot.params, 120.0); // not playing (an export's warm-up)
    CHECK (cap.used == 0);
    fx->setPlaying (true);
    fx->process (l.data(), r.data(), 64, slot.params, 120.0);
    fx->process (l.data(), r.data(), 64, slot.params, 120.0);
    CHECK (cap.used == 128 && cap.left[127] == 0.5f && cap.right[0] == -0.5f);
    CHECK (cap.paramsUsed == 2 && cap.params[1].at == 64 && cap.params[1].values[0] == 0.75f);
    // full: stops (never grows the vectors on the audio thread)
    for (int i = 0; i < 4; ++i)
        fx->process (l.data(), r.data(), 64, slot.params, 120.0);
    CHECK (cap.overflow && cap.used == 256 && cap.left.size() == 256);
    CHECK (visualfx::setCapture (fx.get(), nullptr));
    CHECK (! visualfx::setCapture (makeEffect (EffectKind::eq).get(), &cap));
}

TEST (visual164_spectrum_and_trigger)
{
    using namespace lynx::visualfx::analysis;
    const auto x = visual164::sine (1000.0, 48000.0, 4096);
    std::vector<float> db;
    std::vector<std::complex<float>> work;
    spectrumDb (x.data(), 4096, db, work);
    CHECK (db.size() == 2049);
    const auto peak = (int) (std::max_element (db.begin(), db.end()) - db.begin());
    CHECK (std::abs (peak - (int) std::lround (1000.0 * 4096 / 48000.0)) <= 1);
    CHECK (db[(std::size_t) peak] > -1.6f && db[(std::size_t) peak] < 0.3f); // a full-scale sine ≈ 0 dB (Hann scalloping < 1.5 dB)
    CHECK (db[20] < -60.0f);
    // log bands: the loudest band holds 1 kHz
    std::array<float, 32> bands {};
    logBands (db, 48000.0, 4096, 20.0, 20000.0, bands.data(), 32);
    const auto top = (int) (std::max_element (bands.begin(), bands.end()) - bands.begin());
    const auto lo = 20.0 * std::pow (1000.0, top / 32.0), hi = 20.0 * std::pow (1000.0, (top + 1) / 32.0);
    CHECK (lo <= 1050.0 && hi >= 950.0);
    // tilt: +3 dB/oct around 1 kHz lifts the treble bands
    std::array<float, 32> tilted {};
    logBands (db, 48000.0, 4096, 20.0, 20000.0, tilted.data(), 32, 3.0);
    CHECK (tilted[31] > bands[31] + 9.0f && tilted[0] < bands[0] - 9.0f);
    // the scope's trigger: a rising zero crossing
    const auto y = visual164::sine (100.0, 48000.0, 4800);
    const auto t = triggerPoint (y.data(), 4800, 960);
    CHECK (t <= 4800 - 960 && t > 4800 - 2 * 960 && y[(std::size_t) t - 1] < 0.0f && y[(std::size_t) t] >= 0.0f);
}

TEST (visual164_meter_modes)
{
    using namespace lynx::visualfx::analysis;
    const auto x = visual164::sine (997.0, 48000.0, 48000 * 3);
    auto run = [&] (MeterMode mode) {
        Meter m;
        m.prepare (48000.0);
        for (std::size_t at = 0; at + 512 <= x.size(); at += 512)
            m.process (x.data() + at, x.data() + at, 512, mode, 1.0, 20.0);
        return m;
    };
    CHECK (std::abs (run (MeterMode::peak).levelDb (0)) < 0.05f);
    CHECK (std::abs (run (MeterMode::rms).levelDb (0) + 3.01f) < 0.2f);
    CHECK (std::abs (run (MeterMode::vu).levelDb (1) + 3.01f) < 0.3f);
    CHECK (run (MeterMode::ppm).levelDb (0) > -0.5f);
    // BS.1770: a full-scale 997 Hz sine in both channels = 0 LUFS (−3.01 per channel, summed)
    const auto lufs = run (MeterMode::lufs);
    CHECK (std::abs (lufs.loudness()) < 0.4f);
    // silence afterwards: the peak bar falls at the decay speed, the marker holds first
    Meter m;
    m.prepare (48000.0);
    m.process (x.data(), x.data(), 4800, MeterMode::peak, 1.0, 20.0);
    std::vector<float> quiet (24000, 0.0f);
    m.process (quiet.data(), quiet.data(), 24000, MeterMode::peak, 1.0, 20.0); // 0.5 s
    CHECK (std::abs (m.levelDb (0) + 10.0f) < 0.5f && m.peakDb (0) > -0.1f);
    m.process (quiet.data(), quiet.data(), 24000, MeterMode::peak, 1.0, 20.0);
    m.process (quiet.data(), quiet.data(), 24000, MeterMode::peak, 1.0, 20.0); // hold (1 s) over: it falls too
    CHECK (m.peakDb (0) < -5.0f && m.maxDb() > -0.1f);
}

TEST (visual164_wave_candy_params_and_fl_state)
{
    using namespace lynx;
    namespace wc = visualfx::wavecandy;
    CHECK (effectInfo (EffectKind::waveCandy).params.size() == (std::size_t) wc::count);
    CHECK (effectInfo (EffectKind::visualizer).params.size() == (std::size_t) visualfx::visualizer::count);
    for (int m = 0; m < wc::modeCount; ++m)
        CHECK (wc::modeOf (wc::modeValue ((wc::Mode) m)) == (wc::Mode) m);
    CHECK (wc::resolve (wc::Mode::automatic, 80, 400) == wc::Mode::peakMeter);
    CHECK (wc::resolve (wc::Mode::automatic, 900, 120) == wc::Mode::oscilloscope);
    CHECK (wc::resolve (wc::Mode::automatic, 600, 400) == wc::Mode::spectrum);
    CHECK (wc::resolve (wc::Mode::vectorscope, 80, 400) == wc::Mode::vectorscope);
    CHECK (formatEffectParam (EffectKind::waveCandy, wc::mode, wc::modeValue (wc::Mode::spectrogram), 120.0) == "Spectrogram");
    CHECK (formatEffectParam (EffectKind::waveCandy, wc::gain, 0.5f, 120.0) == "+0.0 dB");
    CHECK (formatEffectParam (EffectKind::waveCandy, wc::specResolution, 0.5f, 120.0) == "4096 points");
    CHECK (formatEffectParam (EffectKind::waveCandy, wc::meterMode, wc::meterModeValue (visualfx::analysis::MeterMode::lufs), 120.0) == "LUFS");
    CHECK (std::abs (wc::windowMs (0.0f) - 2.0) < 1e-6 && std::abs (wc::windowMs (1.0f) - 500.0) < 1e-6);
    CHECK (wc::fftSize (0.0f) == 1024 && wc::fftSize (1.0f) == 16384);

    // FL's state: … [title] [font] [height < 0] [4 bytes] [mode]
    auto flState = [] (const std::string& title, int mode) {
        std::vector<std::uint8_t> s (40, 0);
        s.push_back ((std::uint8_t) title.size());
        s.insert (s.end(), title.begin(), title.end());
        const std::string font = "Trebuchet MS";
        s.push_back ((std::uint8_t) font.size());
        s.insert (s.end(), font.begin(), font.end());
        for (const auto b : { 0xf1, 0xff, 0xff, 0xff, 0x01, 0x02, 0x6e, 0x02 })
            s.push_back ((std::uint8_t) b);
        for (int i = 0; i < 4; ++i)
            s.push_back ((std::uint8_t) (mode >> (8 * i)));
        s.resize (s.size() + 60, 0);
        return s;
    };
    const std::pair<int, wc::Mode> modes[] = { { 0, wc::Mode::oscilloscope }, { 1, wc::Mode::spectrogram }, { 2, wc::Mode::peakMeter },
                                               { 3, wc::Mode::vectorscope } };
    for (const auto& [flMode, mode] : modes)
    {
        auto slot = makeEffectSlot (EffectKind::waveCandy);
        CHECK (wc::fromFl (flState ("Wave Candy", flMode), slot));
        CHECK (wc::modeOf (slot.params[0]) == mode);
        const auto made = effectFromFl ("Wave Candy", flState ("Meter", flMode));
        CHECK (made && made->kind == EffectKind::waveCandy && wc::modeOf (made->params[0]) == mode);
    }
    auto slot = makeEffectSlot (EffectKind::waveCandy);
    CHECK (! wc::fromFl (std::vector<std::uint8_t> (200, 0), slot));
    CHECK (effectForFlPlugin ("Wave Candy") == EffectKind::waveCandy);
    CHECK (effectForFlPlugin ("ZGameEditor Visualizer") == EffectKind::visualizer);
    CHECK (flPluginsForEffect (EffectKind::waveCandy) == std::vector<std::string> { "Wave Candy" });

    // FL's own presets (the installation's Wave Candy "Function" presets)
    if (! realFlpDir.empty())
    {
        const auto dir = std::filesystem::path (realFlpDir) / "Patches" / "Plugin presets" / "Effects" / "Wave Candy" / "Function";
        const std::pair<const char*, wc::Mode> presets[] = { { "Oscilloscope.fst", wc::Mode::oscilloscope }, { "Spectrum.fst", wc::Mode::spectrogram },
                                                             { "Peak meter.fst", wc::Mode::peakMeter }, { "Vectorscope.fst", wc::Mode::vectorscope } };
        for (const auto& [file, mode] : presets)
        {
            std::ifstream in (dir / file, std::ios::binary);
            if (! in)
                continue;
            const std::vector<std::uint8_t> bytes ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());
            const auto fx = effectFromFlPreset (bytes);
            CHECK (fx && fx->kind == EffectKind::waveCandy && wc::modeOf (fx->params[0]) == mode);
        }
    }
}

TEST (visual164_scene_text_round_trip)
{
    using namespace lynx::visualfx::scene;
    for (const auto& preset : presets())
    {
        const auto back = parse (write (preset));
        CHECK (back && *back == preset);
        CHECK (! preset.layers.empty() && preset.layers.front().type == LayerType::background);
    }
    Scene s;
    s.name = "Mine\nline 2";
    s.width = 1080;
    s.height = 1920;
    s.fps = 60;
    Layer t;
    t.type = LayerType::text;
    t.text = "Hello\nWorld \\ back";
    t.path = "C:\\pics\\logo.png";
    t.colourA = 0x80112233;
    t.band = Band::high;
    t.count = 7;
    t.mirror = true;
    s.layers.push_back (t);
    const auto back = fromState (toState (s));
    CHECK (back && *back == s);
    CHECK (! parse ("not a scene"));
    // out-of-range values are clamped
    const auto odd = parse ("LYNX visualizer 1\nsize=99999x3\nfps=500\nlayer=bars\nsize=7\ncount=100000\nopacity=-2\n");
    CHECK (odd && odd->width == 1920 && odd->fps == 120 && odd->layers.size() == 1 && odd->layers[0].size == 1.0f
           && odd->layers[0].count == 512 && odd->layers[0].opacity == 0.0f);
    // a new Visualizer slot carries LYNX's default scene
    const auto slot = lynx::makeEffectSlot (lynx::EffectKind::visualizer);
    CHECK (fromState (slot.state) && *fromState (slot.state) == defaultScene());
}

TEST (visual164_zgameeditor_state)
{
    using namespace lynx::visualfx::scene;
    const std::string ini = "FLhd\x01\x02[General]\r\nGlWindowMode=1\r\nLayerCount=4\r\nLayerOrder=0,2,1,3\r\n"
                            "[AppSet0]\r\nApp=Background\\FourCornerGradient\r\nEnabled=1\r\n"
                            "[AppSet1]\r\nApp=Text\\TextTrueType\r\nEnabled=1\r\n"
                            "[AppSet2]\r\nApp=Peak Effects\\Linear\r\nEnabled=1\r\n"
                            "[AppSet3]\r\nApp=Terrain\\GoopFlow\r\nEnabled=1\r\n"
                            "[Video export]\r\nVideoW=1280\r\nVideoH=720\r\nVideoRenderFps=60\r\n"
                            "[UserContent]\r\nText=\"My song\",\"by me\"\r\nImages=\r\n";
    std::vector<std::string> unmapped;
    const auto s = fromZge ({ ini.begin(), ini.end() }, &unmapped);
    // (round 165e: Terrain maps to a GPU terrain layer)
    CHECK (s && s->layers.size() == 4);
    CHECK (s->layers[0].type == LayerType::background && s->layers[1].type == LayerType::bars && s->layers[2].type == LayerType::text
           && s->layers[3].type == LayerType::terrain);
    CHECK (s->layers[2].text == "My song\nby me");
    CHECK (s->width == 1280 && s->height == 720 && s->fps == 60);
    CHECK (unmapped.empty());
    // only effects without a counterpart: LYNX's default scene (in the project's video size)
    const std::string only = "[General]\nLayerCount=1\n[AppSet0]\nApp=Feedback\\70sKaleido\n[Video export]\nVideoW=640\nVideoH=480\n";
    const auto fallback = fromZge ({ only.begin(), only.end() });
    // (round 165e: the feedback layer stays over the default scene)
    CHECK (fallback && fallback->layers.size() == defaultScene().layers.size() + 1 && fallback->width == 640
           && fallback->layers.back().type == LayerType::feedback);
    CHECK (! fromZge ({ 1, 2, 3 }));
    const auto fx = lynx::effectFromFl ("ZGameEditor Visualizer", { ini.begin(), ini.end() });
    CHECK (fx && fx->kind == lynx::EffectKind::visualizer && fromState (fx->state) && fromState (fx->state)->layers.size() == 4);

    if (! realFlpDir.empty())
    {
        // every FL preset of the plugin is read (an INI), and each gives a scene that draws something
        const auto dir = std::filesystem::path (realFlpDir) / "Patches" / "Plugin presets" / "Effects" / "ZGameEditor Visualizer";
        int read = 0;
        std::error_code ec;
        for (auto it = std::filesystem::recursive_directory_iterator (dir, ec); ! ec && it != std::filesystem::recursive_directory_iterator(); it.increment (ec))
            if (it->path().extension() == ".fst")
            {
                std::ifstream in (it->path(), std::ios::binary);
                const std::vector<std::uint8_t> bytes ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());
                const auto scene = fromZge (bytes);
                CHECK (scene && scene->layers.size() >= 2);
                ++read;
            }
        CHECK (read == 0 || read > 200);
    }
}

TEST (visual164_frames_and_beats)
{
    using namespace lynx::visualfx;
    std::array<float, shownParams> params {};
    params[visualizer::sensitivity] = 0.5f;
    params[visualizer::beatThreshold] = 0.4f;
    params[visualizer::speed] = 0.5f;
    const double rate = 48000.0, fps = 30.0;
    // 120 BPM: a 60 Hz kick burst every half second over a quiet hi-hat noise; 4 s
    const int n = (int) (rate * 4.0);
    std::vector<float> x ((std::size_t) n);
    std::uint32_t noise = 1;
    for (int i = 0; i < n; ++i)
    {
        const auto inBeat = std::fmod ((double) i / rate, 0.5);
        const auto kick = inBeat < 0.12 ? std::sin (2.0 * 3.14159265 * 60.0 * inBeat) * std::exp (-inBeat * 20.0) : 0.0;
        noise = noise * 1664525u + 1013904223u;
        x[(std::size_t) i] = (float) (0.8 * kick + 0.02 * ((double) (noise >> 8) / 16777216.0 - 0.5));
    }
    FrameAnalyzer analyzer;
    int beats = 0;
    float bassMax = 0.0f, highMax = 0.0f;
    for (int frame = 0; frame < (int) (4.0 * fps); ++frame)
    {
        const auto end = std::max (1, (int) (frame / fps * rate));
        const auto& f = analyzer.analyze (x.data(), x.data(), end, rate, frame / fps, 1.0 / fps, params);
        beats += f.onset ? 1 : 0;
        bassMax = std::max (bassMax, f.bass);
        highMax = std::max (highMax, f.high);
        CHECK (f.beat >= 0.0f && f.beat <= 1.0f && f.level >= 0.0f && f.level <= 1.0f);
    }
    CHECK (beats >= 6 && beats <= 9); // 8 kicks (the first may start before the average settles)
    CHECK (bassMax > highMax + 0.2f);
    // silence: nothing moves
    FrameAnalyzer quiet;
    std::vector<float> zero (4096, 0.0f);
    for (int frame = 0; frame < 20; ++frame)
    {
        const auto& f = quiet.analyze (zero.data(), zero.data(), 4096, rate, frame / fps, 1.0 / fps, params);
        CHECK (! f.onset && f.level == 0.0f && f.bass == 0.0f);
    }
    // a quiet mix (-26 dB, 80 Hz) still draws a wave: the waveform's automatic gain brings it up within ~3 s
    {
        FrameAnalyzer agc;
        std::vector<float> sine (8192);
        float peak = 0.0f;
        for (int frame = 0; frame < 120; ++frame)
        {
            for (int i = 0; i < 8192; ++i)
                sine[(std::size_t) i] = 0.05f * (float) std::sin (2.0 * 3.14159265 * 80.0 * ((double) frame * 1600 + i) / rate);
            const auto& f = agc.analyze (sine.data(), sine.data(), 8192, rate, frame / fps, 1.0 / fps, params);
            if (frame == 119)
                for (const auto v : f.waveL)
                    peak = std::max (peak, std::abs (v));
        }
        CHECK (peak > 0.25f && peak < 0.75f);
    }
    // particles: deterministic, a burst on a beat
    scene::Layer layer;
    layer.type = scene::LayerType::particles;
    layer.count = 100;
    ParticleField a, b;
    Frame f;
    f.params = params;
    f.dt = 1.0 / 30.0;
    for (int i = 0; i < 30; ++i)
    {
        f.onset = i == 10;
        f.bass = i >= 10 ? 0.8f : 0.1f;
        const auto before = a.particles().size();
        a.step (f, layer, 16.0f / 9.0f);
        b.step (f, layer, 16.0f / 9.0f);
        if (i == 10)
            CHECK (a.particles().size() > before + 10);
        CHECK (a.particles().size() <= 200);
    }
    CHECK (a.particles().size() == b.particles().size() && a.particles().back().x == b.particles().back().x);
}

TEST (visual164_catalog_visual_category)
{
    using namespace lynx::plugincatalog;
    auto fx = [] (const char* name, const char* category) { return Entry { "id", name, "Vendor", "VST3", category, false }; };
    CHECK (categoryOf (fx ("Plugin Visualizer Pro", "Fx")) == "Visual");
    CHECK (categoryOf (fx ("Mystery", "Fx|Visual")) == "Visual");
    CHECK (categoryOf (fx ("SPAN", "Fx|Analyzer")) == "Analyzer");
    CHECK (std::find (categories (false).begin(), categories (false).end(), "Visual") != categories (false).end());
}
