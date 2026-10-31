// Round 166a, issue #14 (kubapl2004gmail, Linux): the three bugs — snap to scale moves every note, the Piano roll's
// keyboard lights the sounding keys, master / channel pitch on every instrument (REA_rodada166a §2-§4)

TEST (issue14_snap_to_scale_moves_every_note_onto_the_scale)
{
    using namespace lynx;
    int major = 0;
    for (int i = 0; i < (int) notetools::scales().size(); ++i)
        if (std::string (notetools::scales()[(std::size_t) i].name) == "Major")
            major = i;
    CHECK (major > 0);
    Pattern p;
    for (const auto key : { 60, 64, 67 }) // C E G, C major
    {
        Note n;
        n.key = key;
        p.notes.push_back (n);
    }
    const auto originals = p.notes;
    // the mouse moved two semitones up: each note goes to the scale near its own key + 2 (F# → F: a tie goes down)
    notetools::moveKeysOnScale (p, { 0, 1, 2 }, originals, 2, 0, major);
    CHECK (p.notes[0].key == 62 && p.notes[1].key == 65 && p.notes[2].key == 69);
    for (const auto& n : p.notes)
        CHECK (notetools::inScale (n.key, 0, major));
    notetools::moveKeysOnScale (p, { 0, 1, 2 }, originals, 0, 0, major); // no vertical move: left alone
    CHECK (p.notes[0].key == 62);
    // the keyboard's up / down: one scale step
    CHECK (notetools::stepOnScale (64, 1, 0, major) == 65 && notetools::stepOnScale (65, -1, 0, major) == 64);
    CHECK (notetools::stepOnScale (60, 1, 0, major) == 62 && notetools::stepOnScale (61, -1, 0, major) == 60);
    CHECK (notetools::stepOnScale (60, 1, 0, 0) == 61 && notetools::stepOnScale (127, 1, 0, major) == 127);
}

TEST (issue14_renderer_publishes_the_sounding_keys)
{
    layer164::Rig rig (1); // channel 0 = a Layer, channel 1 = a plugin
    CHECK (rig.renderer.soundingKeys (1) == (lynx::NoteMask {}));
    rig.play (1, 60);
    rig.play (1, 100);
    auto k = rig.renderer.soundingKeys (1);
    CHECK ((k[0] >> 60 & 1u) == 1u && (k[1] >> (100 - 64) & 1u) == 1u);
    rig.play (1, 60, false);
    k = rig.renderer.soundingKeys (1);
    CHECK ((k[0] >> 60 & 1u) == 0u && (k[1] >> (100 - 64) & 1u) == 1u);
    CHECK (rig.renderer.soundingKeys (-1) == (lynx::NoteMask {}) && rig.renderer.soundingKeys (5000) == (lynx::NoteMask {}));
}

namespace issue14
{
// a pattern of one channel played by the renderer (Pattern mode, 120 BPM, 48 kHz): the output
std::vector<float> renderPattern (const lynx::ChannelDef& channel, const std::vector<lynx::Note>& notes, float masterPitch, double seconds)
{
    lynx::Song s;
    s.mixer = lynx::Song::defaultMixer (2);
    s.bpm = 120.0;
    s.masterPitch = masterPitch;
    auto c = channel;
    c.mixerTrack = 1;
    s.channels.push_back (c);
    s.patterns.emplace_back();
    s.patterns[0].notes = notes;
    s.patterns[0].lengthTicks = (std::int64_t) (lynx::ppq * 16);
    s.mode = lynx::PlayMode::pattern;
    auto samples = [] (const std::string& path) -> std::shared_ptr<const lynx::SampleData> {
        auto p = path;
        std::replace (p.begin(), p.end(), '\\', '/');
        return lynx::tools::readWav (lynx::existingPathIgnoringCase (p));
    };
    auto state = lynx::RackState::build (s, nullptr, 48000.0, samples);
    lynx::RackRenderer r;
    r.prepare (48000.0, 512);
    const auto n = (int) (seconds * 48000.0);
    std::vector<float> out;
    const auto tps = 120.0 / 60.0 * lynx::ppq / 48000.0;
    for (int done = 0; done < n; done += 512)
    {
        std::vector<float> l (512), rr (512);
        r.process (state.get(), true, done * tps, 512, l.data(), rr.data(), nullptr);
        out.insert (out.end(), l.begin(), l.end());
    }
    return out;
}

// the fundamental (Hz) of out[from, from + 4096) by normalised autocorrelation, 60…1200 Hz; 0 when silent
double pitchAt (const std::vector<float>& x, double seconds)
{
    const auto from = (std::size_t) (seconds * 48000.0);
    const std::size_t w = 4096;
    if (from + w + 1000 > x.size())
        return 0.0;
    double e = 0.0;
    for (std::size_t i = 0; i < w; ++i)
        e += (double) x[from + i] * x[from + i];
    if (e < 1e-6)
        return 0.0;
    std::vector<double> ac (801);
    for (int lag = 40; lag <= 800; ++lag)
    {
        double s = 0.0;
        for (std::size_t i = 0; i < w; ++i)
            s += (double) x[from + i] * x[from + i + (std::size_t) lag];
        ac[(std::size_t) lag] = s / e;
    }
    const auto best = *std::max_element (ac.begin() + 40, ac.end());
    for (int lag = 41; lag < 800; ++lag) // the first peak near the best (no octave-down picks)
        if (ac[(std::size_t) lag] > 0.9 * best && ac[(std::size_t) lag] >= ac[(std::size_t) lag - 1] && ac[(std::size_t) lag] >= ac[(std::size_t) lag + 1])
        {
            const auto a = ac[(std::size_t) lag - 1], b = ac[(std::size_t) lag], c = ac[(std::size_t) lag + 1];
            const auto d = a - 2.0 * b + c;
            return 48000.0 / (lag + (d != 0.0 ? 0.5 * (a - c) / d : 0.0));
        }
    return 0.0;
}

double cents (double a, double b) { return a > 0.0 && b > 0.0 ? 1200.0 * std::log2 (b / a) : 9999.0; }
} // namespace issue14

// An audit of every instrument engine (prints a table): the master pitch, the channel's PITCH knob and slide notes
// Every instrument follows the main pitch, its channel's PITCH knob and slide notes (issue #14 C). Needs FL's data
// (the FL engines' factory presets); LYNX_PITCH_AUDIT=1 also prints the table.
TEST (issue14_pitch_audit_of_the_instruments)
{
    using namespace lynx;
    if (realFlpDir.empty())
        return;
    const auto print = std::getenv ("LYNX_PITCH_AUDIT") != nullptr;
    // the pitch of a guitar model / drums can't be read by the autocorrelation estimate (Drumpad doesn't follow the key)
    const std::set<std::string> unreadable { "FL Slayer", "Fruity Granulizer", "Drumaxx", "Fruit Kick", "BassDrum", "Drumpad" };
    // Plucked! reads Final.Pitch only when a voice starts (REA_rodada65 §1.2: it sizes the delay line): FL can't glide it
    const std::set<std::string> noSlide { "Plucked!" };
    const auto fl = natives142::flAccess();
    std::vector<std::pair<std::string, ChannelDef>> list;
    {
        ChannelDef synth;
        synth.name = "LYNX Synth";
        synth.instrument = InstrumentKind::synth;
        list.emplace_back ("LYNX Synth", synth);
    }
    for (const auto& e : natives::instrumentEngines())
        if (const auto c = natives::makeChannel (e.kind, fl))
            list.emplace_back (e.name, *c);
    Note held;
    held.key = 60;
    held.length = ppq * 8;
    held.velocity = 0.8f;
    Note slide;
    slide.key = 67;
    slide.start = ppq;
    slide.length = ppq / 4;
    slide.slide = true;
    if (print)
        std::printf ("  %-28s %9s %9s %9s %9s\n", "engine", "base Hz", "master", "pitch", "slide");
    for (const auto& [name, c] : list)
    {
        const auto base = issue14::pitchAt (issue14::renderPattern (c, { held }, 0.0f, 1.5), 1.0);
        auto fifth = held; // the reference: the same note played 7 semitones up (the engine's own timbre at that pitch)
        fifth.key = 67;
        const auto ref = issue14::pitchAt (issue14::renderPattern (c, { fifth }, 0.0f, 1.5), 1.0);
        const auto master = issue14::pitchAt (issue14::renderPattern (c, { held }, 700.0f, 1.5), 1.0);
        auto bent = c;
        bent.misc.pitchRange = 12;
        bent.sampler.pitch += 7.0f;
        const auto knob = issue14::pitchAt (issue14::renderPattern (bent, { held }, 0.0f, 1.5), 1.0);
        const auto slid = issue14::pitchAt (issue14::renderPattern (c, { held, slide }, 0.0f, 1.5), 1.0);
        if (print)
            std::printf ("  %-28s %9.1f %+9.0f %+9.0f %+9.0f\n", name.c_str(), base, issue14::cents (base, master), issue14::cents (base, knob),
                         issue14::cents (base, slid));
        if (unreadable.count (name) == 0)
        {
            const auto same = [ref] (double hz) { return std::abs (issue14::cents (ref, hz)) < 40.0; };
            if (! same (master) || ! same (knob) || (noSlide.count (name) == 0 && ! same (slid)))
                std::printf ("  pitch: %s doesn't follow (key +7 %+.0f; master %+.0f, knob %+.0f, slide %+.0f cents)\n", name.c_str(),
                             issue14::cents (base, ref), issue14::cents (base, master), issue14::cents (base, knob), issue14::cents (base, slid));
            CHECK (same (master));
            CHECK (same (knob));
            CHECK (noSlide.count (name) != 0 || same (slid));
        }
    }
}

namespace issue14
{
// a hosted "plugin" that records the live pitch the renderer gives it and the slides it gets
struct PitchSpy final : lynx::Instrument
{
    float live = -99999.0f;
    int slides = 0, slideKey = -1;
    void prepare (double) noexcept override {}
    void noteOn (int, float) noexcept override {}
    void noteOff (int) noexcept override {}
    void allNotesOff() noexcept override {}
    void render (float*, float*, int, float, float) noexcept override {}
    void setLivePitch (float cents) noexcept override { live = cents; }
    void slideTo (int key, float, float) noexcept override
    {
        ++slides;
        slideKey = key;
    }
};
} // namespace issue14

TEST (issue14_plugin_channels_get_the_pitch_knob_main_pitch_and_slides)
{
    using namespace lynx;
    Song song;
    song.mixer = Song::defaultMixer (2);
    song.masterPitch = 50.0f;
    ChannelDef c;
    c.instrument = InstrumentKind::plugin;
    c.plugin.id = "spy";
    c.sampler.pitch = 3.0f;
    c.misc.fineTune = 10.0f;
    song.channels.push_back (c);
    song.patterns.emplace_back();
    auto spy = std::make_shared<issue14::PitchSpy>();
    RackState::Factories f;
    f.pluginInstruments = [spy] (const PluginRef&, double) { return std::shared_ptr<Instrument> (spy); };
    auto state = RackState::build (song, nullptr, 44100.0, f);
    RackRenderer renderer;
    renderer.prepare (44100.0, 256);
    LiveEventQueue live;
    auto play = [&] (NoteEvent::Type type, int key) {
        NoteEvent e;
        e.type = type;
        e.channel = 0;
        e.key = key;
        e.velocity = 0.8f;
        e.slideSamples = 1000.0f;
        live.push (e);
        std::vector<float> l (256), r (256);
        renderer.process (state.get(), false, 0.0, 256, l.data(), r.data(), &live, nullptr);
    };
    play (NoteEvent::Type::on, 60);
    CHECK (std::abs (spy->live - 360.0f) < 0.01f); // 3 semitones + 10 cents fine + 50 cents main pitch

    // "Main pitch" off on the channel: the toolbar knob leaves it alone
    song.channels[0].misc.mainPitch = false;
    state = RackState::build (song, state.get(), 44100.0, f);
    play (NoteEvent::Type::off, 60);
    CHECK (std::abs (spy->live - 310.0f) < 0.01f);

    // a slide note of the pattern reaches the plugin (PluginInstrument turns it into a pitch-bend glide)
    Note on;
    on.key = 60;
    on.length = ppq * 2;
    Note slide;
    slide.key = 64;
    slide.start = ppq / 2;
    slide.length = ppq / 4;
    slide.slide = true;
    song.patterns[0].notes = { on, slide };
    song.patterns[0].lengthTicks = (std::int64_t) (ppq * 4);
    song.mode = PlayMode::pattern;
    state = RackState::build (song, state.get(), 44100.0, f);
    const auto tps = song.bpm / 60.0 * ppq / 44100.0;
    std::vector<float> l (256), r (256);
    for (int done = 0; done < 44100; done += 256)
        renderer.process (state.get(), true, done * tps, 256, l.data(), r.data(), nullptr);
    CHECK (spy->slides == 1 && spy->slideKey == 64);
}
