// Round 142 (issue #5): FL engines usable in native LYNX projects (core/NativeEngines.h, REA_rodada142)

#include "../tools/WavReader.h"
#include "core/NativeEngines.h"
#include "core/VocalPitch.h"

namespace natives142
{
// the user's FL installation as the tests see it (WSL / POSIX): FL's backslashes become '/', names matched ignoring case
lynx::natives::FlAccess flAccess()
{
    namespace fs = std::filesystem;
    lynx::natives::FlAccess fl;
    if (realFlpDir.empty())
        return fl;
    const auto local = [] (std::string p) {
        std::replace (p.begin(), p.end(), '\\', '/');
        return lynx::existingPathIgnoringCase (p);
    };
    fl.roots.data = fs::path (realFlpDir).string();
    fl.roots.factoryData = fs::path (realFlpDir).parent_path().string();
    fl.fileExists = [local] (const std::string& p) { return fs::is_regular_file (local (p)); };
    fl.readFile = [local] (const std::string& p) {
        std::ifstream f (local (p), std::ios::binary);
        return std::vector<std::uint8_t> ((std::istreambuf_iterator<char> (f)), std::istreambuf_iterator<char>());
    };
    fl.listPresets = [local] (const std::string& folder) {
        std::vector<std::string> out;
        std::error_code ec;
        for (const auto& e : fs::recursive_directory_iterator (local (folder), ec))
            if (e.is_regular_file())
                out.push_back (e.path().string());
        std::sort (out.begin(), out.end());
        return out;
    };
    fl.saveEmbedded = [] (const std::vector<std::uint8_t>& wav, const std::string& name) {
        const auto path = (fs::temp_directory_path() / ("lynx-natives142-" + std::to_string (wav.size()) + "-" + std::to_string (name.size()) + ".wav")).string();
        std::ofstream (path, std::ios::binary).write ((const char*) wav.data(), (std::streamsize) wav.size());
        return path;
    };
    return fl;
}

// one channel alone in the rack, a note held for `seconds`: the output's RMS
double renderChannel (const lynx::ChannelDef& channel, const lynx::natives::FlAccess& fl, int key = 60, double seconds = 1.0)
{
    lynx::Song s;
    s.mixer = lynx::Song::defaultMixer (2);
    auto c = channel;
    c.mixerTrack = 1;
    s.channels.push_back (c);
    s.patterns.emplace_back();
    auto samples = [&fl] (const std::string& path) -> std::shared_ptr<const lynx::SampleData> {
        auto p = path;
        std::replace (p.begin(), p.end(), '\\', '/');
        (void) fl;
        return lynx::tools::readWav (lynx::existingPathIgnoringCase (p));
    };
    auto state = lynx::RackState::build (s, nullptr, 48000.0, samples);
    lynx::RackRenderer r;
    r.prepare (48000.0, 512);
    lynx::LiveEventQueue live;
    lynx::NoteEvent on;
    on.channel = 0;
    on.key = key;
    on.velocity = 0.8f;
    live.push (on);
    const auto n = (int) (seconds * 48000.0);
    double sum = 0.0;
    for (int done = 0; done < n; done += 512)
    {
        std::vector<float> l (512), rr (512);
        r.process (state.get(), false, 0.0, 512, l.data(), rr.data(), &live, nullptr);
        for (int i = 0; i < 512; ++i)
            sum += (double) l[(std::size_t) i] * l[(std::size_t) i] + (double) rr[(std::size_t) i] * rr[(std::size_t) i];
    }
    return std::sqrt (sum / (2.0 * n));
}
} // namespace natives142

TEST (natives142_lists_and_no_fl_installation)
{
    using namespace lynx;
    // every engine once; the effect engines are not plain parameter effects
    std::set<int> seen;
    for (const auto& e : natives::instrumentEngines())
        CHECK (seen.insert ((int) e.kind).second);
    seen.clear();
    for (const auto& e : natives::effectEngines())
        CHECK (seen.insert ((int) e.kind).second);
    CHECK (natives::instrumentEngine (InstrumentKind::sytrus) != nullptr && natives::instrumentEngine (InstrumentKind::synth) == nullptr);
    CHECK (natives::effectEngine (EffectKind::newtone) != nullptr && natives::effectEngine (EffectKind::eq) == nullptr);

    // without FL: the engines with LYNX's own defaults are made and play, the others are not available
    natives::FlAccess none;
    for (const auto& e : natives::instrumentEngines())
    {
        const auto c = natives::makeChannel (e.kind, none);
        CHECK (natives::available (e, none) == (e.needs == natives::Needs::nothing));
        CHECK (c.has_value() == (e.needs == natives::Needs::nothing));
        if (c)
        {
            CHECK (c->instrument == e.kind && c->name == e.name);
            CHECK (natives142::renderChannel (*c, none) > 1.0e-3);
        }
    }
    for (const auto& e : natives::effectEngines())
    {
        const auto fx = natives::makeSlot (e.kind, none);
        CHECK (fx.has_value() == (e.needs == natives::Needs::nothing));
        if (fx)
            CHECK (fx->kind == e.kind);
    }
    CHECK (natives::newEffect (EffectKind::eq, none).has_value()); // (a plain effect: always)
    CHECK (natives::effectMenuName (EffectKind::newtime) == "Newtime (beta)" && natives::effectMenuName (EffectKind::newtone) == "Newtone" && natives::effectMenuName (EffectKind::maximus) == "Maximus");
}

TEST (natives142_fl_keys_default_wavetable)
{
    // a wavetable the installation doesn't ship: FL Keys loads the one default.wti names (FL 2026 has no "Piano")
    std::map<std::string, std::string> files {
        { "W\\default.wti", "[default]\r\nfile=mda piano.wti\r\n" },
        { "W\\mda piano.wti", "[info]\r\nname=mda\r\nsamples=1\r\nfiletype=riff\r\nsamplerate=44100\r\n\r\n[velocitylayers]\r\ncount=1\r\n0=0\r\n\r\n[sample0]\r\nname=a.wav\r\nroot=60\r\nhigh=127\r\n" },
    };
    auto read = [&] (const std::string& p) {
        const auto it = files.find (p);
        return it == files.end() ? std::vector<std::uint8_t> {} : std::vector<std::uint8_t> (it->second.begin(), it->second.end());
    };
    auto existing = [] (const std::string& p) { return p; };
    // FL Keys' state: its wavetable name "Piano" (REA_rodada69 §8 layout, see flkeys::parseState)
    lynx::flkeys::State st;
    st.wavetable = "Piano";
    const auto state = lynx::flkeys::writeState (st);
    lynx::ChannelDef def;
    CHECK (lynx::flkeys::importChannel (def, state, "W", read, existing, {}));
    CHECK (def.instrument == lynx::InstrumentKind::flkeys && def.zones.size() == 1 && def.zones[0].path == "W\\a.wav");
    files.erase ("W\\default.wti");
    lynx::ChannelDef none;
    CHECK (! lynx::flkeys::importChannel (none, state, "W", read, existing, {}));
}

TEST (natives142_engines_from_the_fl_installation)
{
    using namespace lynx;
    if (realFlpDir.empty())
        return;
    const auto fl = natives142::flAccess();
    int made = 0;
    for (const auto& e : natives::instrumentEngines())
    {
        const auto c = natives::makeChannel (e.kind, fl);
        CHECK (c.has_value());
        if (! c)
        {
            std::printf ("  not made: %s\n", e.name);
            continue;
        }
        ++made;
        CHECK (c->instrument == e.kind && c->name == e.name);
        if (e.kind == InstrumentKind::granulizer) // (plays the channel's sample: none yet)
            continue;
        const auto rms = natives142::renderChannel (*c, fl, e.kind == InstrumentKind::drumaxx ? 48 : 60);
        CHECK (rms > 1.0e-3 && std::isfinite (rms));
        if (! (rms > 1.0e-3))
            std::printf ("  silent: %s (%g)\n", e.name, rms);
    }
    CHECK (made == (int) natives::instrumentEngines().size());

    for (const auto& e : natives::effectEngines())
    {
        const auto slot = natives::makeSlot (e.kind, fl);
        CHECK (slot.has_value() && slot->kind == e.kind);
        if (! slot)
            continue;
        auto fx = makeEffect (*slot);
        fx->prepare (48000.0);
        double in = 0.0, out = 0.0;
        bool finite = true;
        for (int b = 0; b < 94; ++b)
        {
            std::vector<float> l (512), r (512);
            for (int i = 0; i < 512; ++i)
            {
                const auto t = (b * 512 + i) / 48000.0;
                l[(std::size_t) i] = r[(std::size_t) i] = (float) (0.3 * std::sin (2.0 * M_PI * 220.0 * t) + 0.05 * std::sin (2.0 * M_PI * 3100.0 * t));
                in += 2.0 * l[(std::size_t) i] * l[(std::size_t) i];
            }
            fx->process (l.data(), r.data(), 512, slot->params, 120.0);
            for (int i = 0; i < 512; ++i)
            {
                finite = finite && std::isfinite (l[(std::size_t) i]) && std::isfinite (r[(std::size_t) i]);
                out += (double) l[(std::size_t) i] * l[(std::size_t) i] + (double) r[(std::size_t) i] * r[(std::size_t) i];
            }
        }
        CHECK (finite && out > 0.01 * in);
    }
    // the state-based ones carry FL's default state (not an empty one)
    for (const auto kind : { EffectKind::maximus, EffectKind::vocodex, EffectKind::equo, EffectKind::distructor, EffectKind::lovePhilter, EffectKind::grossBeat })
    {
        const auto slot = natives::makeSlot (kind, fl);
        CHECK (slot && ! slot->state.empty());
    }
}

TEST (natives142_newtone_and_newtime_native_projects)
{
    using namespace lynx;
    // a "sung" line: four notes (A3, B3, C4, rest, E4) with a light vibrato
    SampleData s;
    s.sampleRate = 44100.0;
    const double hz[] = { 220.0, 246.94, 261.63, 0.0, 329.63 };
    double ph = 0.0;
    for (const auto f0 : hz)
        for (int i = 0; i < 17640; ++i)
        {
            const auto f = f0 * (1.0 + 0.004 * std::sin (2.0 * M_PI * 5.5 * i / 44100.0));
            ph += 2.0 * M_PI * f / 44100.0;
            float v = 0.0f;
            if (f0 > 0.0)
                for (int h = 1; h < 8; ++h)
                    v += (float) (0.3 / h * std::sin (h * ph));
            s.left.push_back (v);
        }

    // the analysis finds the four notes at their pitches, nothing edited
    auto t = newtonefx::analyseNotes (s);
    CHECK (t.pitches.size() == 4 && t.frames == (std::int64_t) s.frames());
    const double expected[] = { 57.0, 59.0, 60.0, 64.0 };
    for (std::size_t k = 0; k < std::min<std::size_t> (4, t.pitches.size()); ++k)
    {
        CHECK (std::abs (t.pitches[k].origPitch - expected[k]) < 0.1);
        CHECK (! t.pitches[k].edited (1.0) && t.pitches[k].curStart == t.pitches[k].origStart);
    }
    CHECK (newtonefx::renderEdits (s, t) == nullptr); // (unedited: the source plays as it is)

    // LYNX's own state round-trips; trackOf reads it
    auto state = newtonefx::writeNative (t);
    auto back = newtonefx::parseNative (state);
    CHECK (back && back->pitches.size() == t.pitches.size() && ! back->edited);
    CHECK (newtonefx::trackOf (state).has_value() && ! newtonefx::parseNative ({ 1, 2, 3 }));
    state.push_back (0);
    CHECK (! newtonefx::parseNative (state)); // (trailing bytes: not ours)

    // a note moved up two semitones sounds there; the slot plays the edited sample in slave mode
    if (t.pitches.size() == 4)
    {
        t.pitches[1].shift = 2.0;
        const auto edited = newtonefx::writeNative (t);
        CHECK (newtonefx::parseNative (edited)->edited);
        auto src = std::make_shared<SampleData> (s);
        const auto rendered = newtonefx::renderedSource (src, edited);
        CHECK (rendered != nullptr && rendered != src && rendered->frames() == s.frames());
        const auto frames = vocal::analysePitch (rendered->leftData() + 22050, 8000, 44100.0, 512);
        double note = 0.0;
        for (const auto& f : frames)
            if (f.hz > 0.0)
            {
                note = vocal::hzToNote (f.hz);
                break;
            }
        CHECK (std::abs (note - 61.0) < 0.3);

        auto slot = makeEffectSlot (EffectKind::newtone);
        slot.params[0] = 1.0f;
        slot.state = edited;
        auto fx = newtonefx::make (src, slot.state);
        fx->prepare (44100.0);
        fx->setPlaying (true);
        fx->setSongPosition (0.0);
        std::vector<float> l (512, 0.0f), r (512, 0.0f);
        fx->process (l.data(), r.data(), 512, slot.params, 120.0);
        CHECK (std::abs (l[100] - rendered->leftData()[100]) < 1e-6f);
    }

    // Newtime: a marker stretches the first second to 1.5 s; the rest follows; the map round-trips
    newtonefx::WarpMap m;
    m.frames = (std::int64_t) s.frames();
    m.markers = { { 44100, 66150 } };
    CHECK (newtonefx::warpedLength (m, m.frames) == 66150 + (m.frames - 44100) * 3 / 2); // (the last slope goes on)
    CHECK (std::abs (newtonefx::warpToOutput (m, m.frames, 22050.0) - 33075.0) < 1e-6);
    CHECK (std::abs (newtonefx::warpToSource (m, m.frames, 33075.0) - 22050.0) < 1e-6);
    const auto warped = newtonefx::renderWarp (s, m);
    CHECK (warped != nullptr && (std::int64_t) warped->frames() == newtonefx::warpedLength (m, m.frames));
    const auto map = newtonefx::parseWarp (newtonefx::writeWarp (m));
    CHECK (map && *map == m);
    newtonefx::WarpMap bad = m;
    bad.markers.push_back ({ 40000, 70000 }); // (not increasing)
    CHECK (! newtonefx::parseWarp (newtonefx::writeWarp (bad)));
    CHECK (newtonefx::renderWarp (s, newtonefx::WarpMap {}) == nullptr);
}
