// --- Round 162 (issue #9): channel routing past insert 16, sampler fine tune / stretch / pitch wheel ---------------

namespace s162
{
// a mono sine sample
std::shared_ptr<lynx::SampleData> sine (double freq, double seconds, double rate = 44100.0)
{
    auto s = std::make_shared<lynx::SampleData>();
    s->sampleRate = rate;
    const auto n = (std::size_t) (seconds * rate);
    for (std::size_t i = 0; i < n; ++i)
        s->left.push_back ((float) (0.5 * std::sin (2.0 * 3.141592653589793 * freq * (double) i / rate)));
    return s;
}

// frequency of the steady part of `x` (positive-going zero crossings between 30 % and 70 % of the sounding part) and
// the number of sounding samples (until the signal stays below -60 dB)
struct Heard
{
    double freq = 0.0;
    long length = 0;
};

Heard analyse (const std::vector<float>& x, double rate)
{
    Heard h;
    long last = -1;
    for (long i = 0; i < (long) x.size(); ++i)
        if (std::abs (x[(std::size_t) i]) > 0.001f)
            last = i;
    h.length = last + 1;
    const auto from = (long) (h.length * 0.3), to = (long) (h.length * 0.7);
    long first = -1, end = -1;
    int crossings = 0;
    for (long i = from + 1; i < to; ++i)
        if (x[(std::size_t) i - 1] <= 0.0f && x[(std::size_t) i] > 0.0f)
        {
            if (first < 0)
                first = i;
            else
                ++crossings, end = i;
        }
    if (crossings > 0)
        h.freq = crossings * rate / (double) (end - first);
    return h;
}

// a sampler channel playing `sample` with `settings`: one live note (key, cents of fine tune), the transport
// stopped, rendered until it ends (at most `seconds`)
Heard playLive (const lynx::SamplerSettings& settings, std::shared_ptr<lynx::SampleData> sample, int key, float fineCents = 0.0f,
                double seconds = 6.0, float bend = 0.0f, int range = 2)
{
    lynx::Song s;
    s.mixer = lynx::Song::defaultMixer (2);
    s.bpm = 120.0;
    lynx::ChannelDef c;
    c.instrument = lynx::InstrumentKind::sampler;
    c.samplePath = "sine.wav";
    c.sampler = settings;
    c.misc.fineTune = fineCents;
    c.misc.pitchRange = range;
    c.mixerTrack = 1;
    s.channels.push_back (c);
    s.patterns.emplace_back();
    lynx::RackState::Factories f;
    f.samples = [sample] (const std::string&) { return std::shared_ptr<const lynx::SampleData> (sample); };
    f.stretchedSamples = [sample] (const std::string&, double ratio, lynx::StretchMethod m) {
        return std::shared_ptr<const lynx::SampleData> (lynx::timeStretch (*sample, ratio, m));
    };
    constexpr double rate = 44100.0;
    auto state = lynx::RackState::build (s, nullptr, rate, f);
    lynx::RackRenderer r;
    r.prepare (rate, 441);
    lynx::LiveEventQueue live;
    if (bend != 0.0f)
    {
        lynx::NoteEvent b;
        b.type = lynx::NoteEvent::Type::control;
        b.channel = 0;
        b.key = lynx::NoteEvent::controlPitchBend;
        b.velocity = bend;
        live.push (b);
    }
    lynx::NoteEvent e;
    e.channel = 0;
    e.key = key;
    e.velocity = 0.8f;
    live.push (e);
    std::vector<float> out, l (441), rr (441);
    for (int b = 0; b < (int) (seconds * 100.0); ++b)
    {
        r.process (state.get(), false, 0.0, 441, l.data(), rr.data(), &live, nullptr);
        out.insert (out.end(), l.begin(), l.end());
    }
    return analyse (out, rate);
}
} // namespace s162

TEST (s162_a_channel_routes_to_any_insert)
{
    using namespace lynx;
    auto s = Song::makeDefault();
    CHECK (s.mixer.size() == 17); // Master + 16, as before
    s.channels.resize (std::max<std::size_t> (s.channels.size(), 1));
    // issue #9: past insert 16 the route box stopped; now the inserts up to the target are added
    mixerops::routeChannel (s, 0, 64);
    CHECK (s.channels[0].mixerTrack == 64);
    CHECK (s.mixer.size() == 65);
    CHECK (s.mixer[40].name == "Insert 40" && s.mixer[64].name == "Insert 64");
    mixerops::routeChannel (s, 0, 500); // at most FL's last insert
    CHECK (s.channels[0].mixerTrack == maxMixerTracks - 1);
    CHECK ((int) s.mixer.size() == maxMixerTracks);
    mixerops::routeChannel (s, 0, 3); // routing lower never removes inserts
    CHECK (s.channels[0].mixerTrack == 3 && (int) s.mixer.size() == maxMixerTracks);

    // a new channel takes the next free insert, past the 16th when they are all in use (it used to fall to Master)
    auto t = Song::makeDefault();
    t.channels.clear();
    for (int i = 0; i < 20; ++i)
    {
        ChannelDef c;
        c.mixerTrack = t.claimFreeInsert();
        t.channels.push_back (c);
    }
    CHECK (t.channels[15].mixerTrack == 16);
    CHECK (t.channels[16].mixerTrack == 17 && t.channels[19].mixerTrack == 20);
    CHECK (t.mixer.size() == 21);
    CHECK (t.nextFreeInsert() == 0); // (the const query still says "none free")
}

TEST (s162_the_rack_mixes_a_channel_routed_past_insert_16_into_that_insert)
{
    using namespace lynx;
    Song s;
    s.mixer = Song::defaultMixer (16);
    ChannelDef c;
    c.instrument = InstrumentKind::synth;
    s.channels.push_back (c);
    mixerops::routeChannel (s, 0, 40);
    CHECK (s.mixer.size() == 41);
    // the insert gets the channel (not insert 16, where the renderer used to clamp it)
    CHECK (mixerops::channelsOn (s, 40).size() == 1);
    CHECK (mixerops::channelsOn (s, 16).empty());
}

TEST (s162_sampler_fine_tune_changes_live_notes)
{
    using namespace lynx;
    const auto sample = s162::sine (440.0, 1.0);
    SamplerSettings st;
    const auto plain = s162::playLive (st, sample, 60);
    CHECK (std::abs (plain.freq - 440.0) < 1.0);
    // Misc › Fine tune +50 cents on a key played live: 440 · 2^(50/1200) = 452.9 Hz (it was ignored)
    const auto fine = s162::playLive (st, sample, 60, 50.0f);
    CHECK (std::abs (fine.freq - 452.9) < 1.5);
    // the sampler's pitch knob, in cents (-0.25 st)
    st.pitch = -0.25f;
    CHECK (std::abs (s162::playLive (st, sample, 60).freq - 440.0 * std::exp2 (-0.25 / 12.0)) < 1.0);
}

TEST (s162_resample_stretch_follows_the_keys)
{
    using namespace lynx;
    // a 1 s sample stretched (Resample) to 4 beats = 2 s at 120 BPM: an octave down at the root key
    const auto sample = s162::sine (440.0, 1.0);
    SamplerSettings st;
    st.stretchBeats = 4.0f;
    const auto root = s162::playLive (st, sample, 60);
    CHECK (std::abs (root.freq - 220.0) < 1.0);
    CHECK (std::abs (root.length - 88200) < 400);
    // FL's Resample: pitch and length change together, like tape (LYNX played every key at the root's pitch)
    const auto fifth = s162::playLive (st, sample, 67);
    CHECK (std::abs (fifth.freq - 220.0 * std::exp2 (7.0 / 12.0)) < 1.5);
    CHECK (std::abs (fifth.length - 88200.0 / std::exp2 (7.0 / 12.0)) < 400);
    // fine tune and pitch knob too
    CHECK (std::abs (s162::playLive (st, sample, 60, 100.0f).freq - 220.0 * std::exp2 (1.0 / 12.0)) < 1.0);
}

TEST (s162_stretch_keeping_pitch_transposes_and_keeps_the_length)
{
    using namespace lynx;
    const auto sample = s162::sine (440.0, 1.0);
    SamplerSettings st;
    st.stretchBeats = 4.0f; // 2 s at 120 BPM
    st.keepPitch = true;
    const auto root = s162::playLive (st, sample, 60);
    CHECK (std::abs (root.freq - 440.0) < 1.5);
    CHECK (std::abs (root.length - 88200) < 2000);
    // FL's Stretch modes: "pitch changes keep the original duration" — the key transposes, the length stays
    for (const int key : { 67, 55, 72 })
    {
        const auto heard = s162::playLive (st, sample, key);
        const auto expected = 440.0 * std::exp2 ((key - 60) / 12.0);
        CHECK (std::abs (heard.freq - expected) < expected * 0.004);
        CHECK (std::abs (heard.length - root.length) < 4000);
    }
    // the fine tune moves it too
    const auto fine = s162::playLive (st, sample, 60, 100.0f);
    CHECK (std::abs (fine.freq - 440.0 * std::exp2 (1.0 / 12.0)) < 2.5);
}

TEST (s162_pitch_wheel_bends_native_instruments_by_the_pitch_range)
{
    using namespace lynx;
    const auto sample = s162::sine (440.0, 1.0);
    SamplerSettings st;
    // full wheel up with FL's default range (2 semitones): 440 · 2^(2/12)
    CHECK (std::abs (s162::playLive (st, sample, 60, 0.0f, 3.0, 1.0f).freq - 440.0 * std::exp2 (2.0 / 12.0)) < 1.5);
    // half down with a 12 semitone range: -6 st
    CHECK (std::abs (s162::playLive (st, sample, 60, 0.0f, 3.0, -0.5f, 12).freq - 440.0 * std::exp2 (-6.0 / 12.0)) < 1.0);
    // stretch keeping pitch: the wheel bends the shifter (the length stays)
    st.stretchBeats = 4.0f;
    st.keepPitch = true;
    const auto bent = s162::playLive (st, sample, 64, 0.0f, 4.0, 1.0f);
    CHECK (std::abs (bent.freq - 440.0 * std::exp2 (6.0 / 12.0)) < 3.0);
    CHECK (std::abs (bent.length - 88200) < 4000);

    // the built-in synth: a sine at C5 bent up 2 semitones
    auto synth = makeInstrument (InstrumentKind::synth);
    synth->prepare (44100.0);
    ChannelDef c;
    c.synth.osc[0] = { Waveform::sine, 0, 0.0f, 1.0f };
    c.synth.osc[1].level = c.synth.osc[2].level = 0.0f;
    synth->configure (c, 120.0);
    synth->pitchBend (2.0f);
    synth->noteOn (69, 0.8f);
    std::vector<float> l (44100, 0.0f), r (44100, 0.0f);
    synth->render (l.data(), r.data(), 44100, 1.0f, 1.0f);
    CHECK (std::abs (s162::analyse (l, 44100.0).freq - 440.0 * std::exp2 (2.0 / 12.0)) < 2.0);
}

namespace s162
{
// a fake hosted plugin: records the MIDI messages it gets
struct MidiSpy final : lynx::Instrument
{
    std::vector<std::vector<std::uint8_t>> got;
    void prepare (double) noexcept override {}
    void noteOn (int, float) noexcept override {}
    void noteOff (int) noexcept override {}
    void allNotesOff() noexcept override {}
    void render (float*, float*, int, float, float) noexcept override {}
    void midiMessage (const std::uint8_t* bytes, int size) noexcept override { got.emplace_back (bytes, bytes + size); }
};
} // namespace s162

TEST (s162_pitch_and_mod_wheel_reach_a_hosted_plugin)
{
    using namespace lynx;
    Song s;
    s.mixer = Song::defaultMixer (2);
    ChannelDef c;
    c.instrument = InstrumentKind::plugin;
    c.plugin.id = "spy";
    s.channels.push_back (c);
    s.patterns.emplace_back();
    auto spy = std::make_shared<s162::MidiSpy>();
    RackState::Factories f;
    f.pluginInstruments = [spy] (const PluginRef&, double) { return std::shared_ptr<Instrument> (spy); };
    auto state = RackState::build (s, nullptr, 44100.0, f);
    RackRenderer r;
    r.prepare (44100.0, 256);
    LiveEventQueue live;
    NoteEvent e;
    e.type = NoteEvent::Type::control;
    e.channel = 0;
    e.key = NoteEvent::controlPitchBend;
    e.velocity = 1.0f;
    live.push (e);
    e.velocity = 0.0f;
    live.push (e);
    e.key = 1; // mod wheel
    e.velocity = 64.0f / 127.0f;
    live.push (e);
    std::vector<float> l (256), rr (256);
    r.process (state.get(), false, 0.0, 256, l.data(), rr.data(), &live, nullptr);
    CHECK (spy->got.size() == 3);
    if (spy->got.size() == 3)
    {
        CHECK ((spy->got[0] == std::vector<std::uint8_t> { 0xE0, 127, 127 }));
        CHECK ((spy->got[1] == std::vector<std::uint8_t> { 0xE0, 0, 64 }));
        CHECK ((spy->got[2] == std::vector<std::uint8_t> { 0xB0, 1, 64 }));
    }
}
