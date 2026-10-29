// Audit round 165z-a2: the metronome as FL Studio 2026 plays it (core/MetronomeVoice.h; docs/analise/REA_rodada165z-a.md
// item 4.10). Included at the end of tests.cpp.

TEST (audit165za2_metronome_sounds_follow_fl_menu)
{
    namespace m = lynx::metronome;
    // FL's right-click menu: Hi-hat, Tick, Beep, Cowbell, Pulse; the factory default is Tick ("Metronome number" = 1)
    CHECK (m::soundCount == 5);
    CHECK (std::string (m::soundName (0)) == "Hi-hat" && std::string (m::soundName (1)) == "Tick" && std::string (m::soundName (2)) == "Beep"
           && std::string (m::soundName (3)) == "Cowbell" && std::string (m::soundName (4)) == "Pulse");
    CHECK (m::defaultSound == 1);
    lynx::Transport transport;
    CHECK (transport.metronomeSound.load() == m::defaultSound && transport.metronomeTrack.load() == 0);
    // FL's files in Data\System
    CHECK (std::string (m::soundFileName (0)) == "Metronome.wav" && std::string (m::soundFileName (1)) == "Metronome2.wav"
           && std::string (m::soundFileName (4)) == "Metronome5.wav");
    CHECK (m::clampSound (-1) == m::defaultSound && m::clampSound (5) == m::defaultSound && m::clampSound (3) == 3);

    // ui-options.txt before 0.1.20: 0 LYNX click (gone: FL's default Tick), 1 hi-hat .. 5 pulse keep their sound
    CHECK (m::soundFromLegacy (0) == 1);
    CHECK (m::soundFromLegacy (1) == 0 && m::soundFromLegacy (2) == 1 && m::soundFromLegacy (3) == 2 && m::soundFromLegacy (4) == 3
           && m::soundFromLegacy (5) == 4);
    CHECK (m::soundFromLegacy (9) == 1 && m::soundFromLegacy (-3) == 1);

    // "Metronome track": "---" (0) is the Master; an insert this song lacks falls back to the Master
    CHECK (m::targetTrack (0, 17) == 0 && m::targetTrack (5, 17) == 5 && m::targetTrack (17, 17) == 0 && m::targetTrack (-2, 17) == 0);
}

TEST (audit165za2_metronome_levels_measured_in_fl)
{
    namespace m = lynx::metronome;
    // a ramp stands in for a sound: the output shows the gain, the pitch and the interpolation
    m::Sample s;
    s.rate = 44100.0;
    for (int i = 0; i < 100; ++i)
        s.left.push_back ((float) i / 100.0f);
    s.right = s.left;

    m::Voice v;
    v.setOutputRate (44100.0);
    v.trigger (&s, false);
    std::vector<float> l, r;
    for (float a = 0.0f, b = 0.0f; v.next (1.0f, a, b); a = b = 0.0f)
    {
        l.push_back (a);
        r.push_back (b);
    }
    // a beat: the sample at its own pitch × 0.6233 (−4.11 dB, FL's Audio logger)
    CHECK (l.size() == 100);
    CHECK (std::abs (l[50] - 0.6233f * 0.5f) < 1e-5f && r[50] == l[50]);

    // the downbeat: two semitones up (1.1225 × faster, linear interpolation) × 0.9404 (−0.53 dB)
    v.trigger (&s, true);
    l.clear();
    for (float a = 0.0f, b = 0.0f; v.next (1.0f, a, b); a = b = 0.0f)
        l.push_back (a);
    CHECK (l.size() == 90); // ceil (100 / 1.1225)
    CHECK (std::abs (l[40] - 0.9404f * (40.0f * (float) m::downbeatPitch / 100.0f)) < 1e-4f);

    // at 48 kHz the 44.1 kHz file keeps its pitch (and length in time)
    v.setOutputRate (48000.0);
    v.trigger (&s, false);
    int frames = 0;
    for (float a = 0.0f, b = 0.0f; v.next (0.5f, a, b); a = b = 0.0f)
        ++frames;
    CHECK (frames == 109); // ceil (100 × 48000 / 44100)

    // the volume trim scales it; a new click cuts the one before
    v.setOutputRate (44100.0);
    v.trigger (&s, false);
    float a = 0.0f, b = 0.0f;
    for (int i = 0; i < 60; ++i)
    {
        a = b = 0.0f;
        v.next (0.5f, a, b);
    }
    CHECK (std::abs (a - 0.5f * 0.6233f * 0.59f) < 1e-5f);
    v.trigger (&s, false);
    a = b = 0.0f;
    v.next (1.0f, a, b);
    CHECK (a == 0.0f && v.playing());

    // LYNX's stand-ins without FL: the length and peak of FL's files (frames at 44.1 kHz)
    const int frameCounts[] = { 4612, 657, 3000, 9467, 67 };
    const float peaks[] = { 0.8f, 1.0f, 0.645f, 1.0f, 0.488f };
    for (int i = 0; i < m::soundCount; ++i)
    {
        const auto f = m::fallbackSound (i);
        CHECK (f.frames() == frameCounts[i] && ! f.fromFl && f.right == f.left);
        float peak = 0.0f;
        for (const auto x : f.left)
            peak = std::max (peak, std::abs (x));
        CHECK (std::abs (peak - peaks[i]) < 1e-4f);
    }
}

TEST (audit165za2_metronome_plays_through_its_mixer_track)
{
    // FL's Settings › Audio › "Metronome track": "---" plays into the Master, an insert plays it through that insert
    // (its fader and mute), the Master when the insert does not exist
    auto song = lynx::Song::makeDefault();
    song.channels.clear();
    song.playlist.clear();
    for (auto& p : song.patterns)
        p.notes.clear();
    song.mixer = lynx::Song::defaultMixer (4);
    auto state = lynx::RackState::build (song, nullptr, 48000.0);

    lynx::RackRenderer renderer;
    renderer.prepare (48000.0, 512);
    std::vector<float> clickL (256, 0.5f), clickR (256, -0.25f), l (256), r (256);

    auto run = [&] (const lynx::RackState* st, int track, int blocks = 12) {
        for (int i = 0; i < blocks; ++i) // past the fader ramps
        {
            renderer.setMetronomeInput (clickL.data(), clickR.data(), track);
            renderer.process (st, false, 0.0, 256, l.data(), r.data());
        }
    };
    run (state.get(), 0);
    CHECK (std::abs (l[100] - 0.5f) < 1e-3f && std::abs (r[100] + 0.25f) < 1e-3f);
    run (state.get(), 3);
    CHECK (std::abs (l[100] - 0.5f) < 1e-3f && std::abs (r[100] + 0.25f) < 1e-3f);
    run (state.get(), 40); // no insert 40 in this song: the Master
    CHECK (std::abs (l[100] - 0.5f) < 1e-3f);

    // no click: silence
    renderer.setMetronomeInput (nullptr, nullptr, 0);
    renderer.process (state.get(), false, 0.0, 256, l.data(), r.data());
    CHECK (std::abs (l[100]) < 1e-6f);

    // through insert 3's fader (half way to 0 dB, FL's fader curve) and mute
    song.mixer[3].volume = lynx::unityFader * 0.5f;
    const auto halfFader = lynx::faderGain (lynx::unityFader * 0.5f);
    CHECK (halfFader > 0.01f && halfFader < 0.9f);
    auto half = lynx::RackState::build (song, state.get(), 48000.0);
    run (half.get(), 3);
    CHECK (std::abs (l[100] - 0.5f * halfFader) < 2e-3f);
    run (half.get(), 0); // (the Master is not insert 3)
    CHECK (std::abs (l[100] - 0.5f) < 1e-3f);
    song.mixer[3].muted = true;
    auto muted = lynx::RackState::build (song, half.get(), 48000.0);
    run (muted.get(), 3, 20);
    CHECK (std::abs (l[100]) < 1e-6f);

    // the Master's fader applies to it too (it goes into the Master's input)
    song.mixer[3].muted = false;
    song.mixer[0].volume = lynx::unityFader * 0.5f;
    auto master = lynx::RackState::build (song, muted.get(), 48000.0);
    run (master.get(), 0);
    CHECK (std::abs (l[100] - 0.5f * halfFader) < 2e-3f);
}
