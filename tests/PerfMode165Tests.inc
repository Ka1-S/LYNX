// Round 165m: FL's Performance mode (core/Performance.h) and Patcherize.

namespace perfmode165
{
// A song with a performance zone of 4 bars (Start marker at bar 5), track 0: three one-bar pattern clips in the zone
// (patterns 0, 1, 2, one note each on their first beat with keys 60, 62, 64) and track 1: one clip after the zone.
inline lynx::Song zoneSong()
{
    using namespace lynx;
    Song s;
    s.bpm = 120.0;
    s.channels.resize (1);
    for (int p = 0; p < 3; ++p)
    {
        auto& pattern = s.patterns.emplace_back();
        pattern.name = "P" + std::to_string (p);
        Note n;
        n.key = 60 + 2 * p;
        n.length = ppq / 2;
        pattern.notes.push_back (n);
        pattern.lengthTicks = 4 * ppq;
    }
    for (int k = 0; k < 3; ++k)
    {
        Clip c;
        c.pattern = k;
        c.track = 0;
        c.start = k * 4 * ppq;
        c.length = 4 * ppq;
        s.playlist.push_back (c);
    }
    Clip after;
    after.pattern = 0;
    after.track = 1;
    after.start = 16 * ppq;
    after.length = 4 * ppq;
    s.playlist.push_back (after);
    Marker start;
    start.tick = 16 * ppq;
    start.action = Marker::songStart;
    s.addMarker (start);
    s.mode = PlayMode::song;
    s.performanceMode = true;
    s.playlistTracks.resize (2);
    return s;
}

struct Run
{
    lynx::Song song;
    std::vector<int> zone;
    lynx::perf::Live live;
    lynx::perf::Player player;
    double clock = 0.0;
    static constexpr double block = 120.0; // ticks per block

    explicit Run (lynx::Song s) : song (std::move (s)), zone (lynx::perf::zoneClips (song)) {}

    void command (lynx::perf::Command::Type type, int track, std::int64_t clipStart)
    {
        lynx::perf::Command c;
        c.type = type;
        c.track = track;
        c.clipStart = clipStart;
        live.commands.push (c);
    }
    // runs blocks until the clock reaches `until`
    void runTo (double until)
    {
        while (clock < until - 1.0e-9)
        {
            player.process (song, zone, &live, clock, clock, block, true);
            clock += block;
        }
    }
    std::int64_t playing (int track) const { return player.playingOn (track); }
};
} // namespace perfmode165

TEST (perfmode165_zone_from_start_marker_and_song_skips_it)
{
    using namespace lynx;
    auto s = perfmode165::zoneSong();
    CHECK (s.performanceZoneEnd() == 16 * ppq);
    CHECK (perf::zoneClips (s).size() == 3);
    const auto tps = ticksPerSample (120.0, 48000.0);
    EventBuffer ev;
    scheduleSong (s, 0.0, (int) (16 * ppq / tps), tps, ev);
    CHECK (ev.size() == 0); // the zone's clips don't play with the song
    s.performanceMode = false;
    CHECK (s.performanceZoneEnd() == 0);
    ev.clear();
    scheduleSong (s, 0.0, (int) (16 * ppq / tps), tps, ev);
    CHECK (ev.size() == 6); // off: three notes on + off
    // the song loops back to the Start marker in Performance mode
    const auto loop = playbackLoop (true, 0.0, 0.0, 20.0 * ppq, 16.0 * ppq);
    CHECK (loop.start == 16.0 * ppq && loop.end == 20.0 * ppq);
}

TEST (perfmode165_trigger_sync_tolerant_and_queue)
{
    using namespace lynx;
    using T = perf::Command::Type;
    {
        perfmode165::Run r (perfmode165::zoneSong()); // FL defaults: 4 beats, tolerant
        r.runTo (600.0);                      // 600 ticks in: past the tolerance (1/8 of 4 beats = 480)
        r.command (T::press, 0, 4 * ppq);
        r.runTo (720.0);
        CHECK (r.playing (0) == -1); // waits for the next 4-beat point
        CHECK (r.live.tracks[0].pending.load() == 4 * ppq);
        r.runTo (4.0 * ppq + 120.0);
        CHECK (r.playing (0) == 4 * ppq);
    }
    {
        perfmode165::Run r (perfmode165::zoneSong());
        r.runTo (4.0 * ppq + 240.0); // a quarter beat late: tolerated, starts now, a quarter beat into the clip
        r.command (T::press, 0, 0);
        r.runTo (4.0 * ppq + 360.0);
        CHECK (r.playing (0) == 0);
        CHECK (std::abs (r.live.tracks[0].progress.load() - 240.0f / (4 * ppq)) < 0.01f);
    }
    {
        auto s = perfmode165::zoneSong();
        s.playlistTracks[0].performance.tolerant = false;
        perfmode165::Run r (std::move (s));
        r.runTo (4.0 * ppq + 240.0);
        r.command (T::press, 0, 0);
        r.runTo (4.0 * ppq + 360.0);
        CHECK (r.playing (0) == -1); // not tolerant: the next grid point
        r.runTo (8.0 * ppq + 120.0);
        CHECK (r.playing (0) == 0);
    }
    {
        auto s = perfmode165::zoneSong();
        s.playlistTracks[0].performance.triggerSync = TrackPerformance::Sync::off;
        s.playlistTracks[0].performance.queue = true;
        perfmode165::Run r (std::move (s));
        r.command (T::press, 0, 0);
        r.runTo (240.0);
        CHECK (r.playing (0) == 0);
        r.command (T::press, 0, 8 * ppq); // queued: after the playing clip's pass (4 beats)
        r.runTo (2.0 * ppq);
        CHECK (r.playing (0) == 0);
        r.runTo (4.0 * ppq + 120.0);
        CHECK (r.playing (0) == 8 * ppq);
    }
}

TEST (perfmode165_motions)
{
    using namespace lynx;
    using T = perf::Command::Type;
    using M = TrackPerformance::Motion;
    auto with = [] (M motion) {
        auto s = perfmode165::zoneSong();
        s.playlistTracks[0].performance.motion = motion;
        s.playlistTracks[0].performance.triggerSync = TrackPerformance::Sync::off;
        return s;
    };
    {
        perfmode165::Run r (with (M::stay));
        r.command (T::press, 0, 0);
        r.runTo (13.0 * ppq);
        CHECK (r.playing (0) == 0); // loops on
    }
    {
        perfmode165::Run r (with (M::oneShot));
        r.command (T::press, 0, 0);
        r.runTo (2.0 * ppq);
        CHECK (r.playing (0) == 0);
        r.runTo (5.0 * ppq);
        CHECK (r.playing (0) == -1); // once
    }
    {
        perfmode165::Run r (with (M::marchWrap));
        r.command (T::press, 0, 0);
        r.runTo (5.0 * ppq);
        CHECK (r.playing (0) == 4 * ppq);
        r.runTo (9.0 * ppq);
        CHECK (r.playing (0) == 8 * ppq);
        r.runTo (13.0 * ppq);
        CHECK (r.playing (0) == 0); // wraps to the first
    }
    {
        perfmode165::Run r (with (M::marchStay));
        r.command (T::press, 0, 8 * ppq);
        r.runTo (9.0 * ppq);
        CHECK (r.playing (0) == 8 * ppq); // the last one stays
    }
    {
        perfmode165::Run r (with (M::marchStop));
        r.command (T::press, 0, 4 * ppq);
        r.runTo (5.0 * ppq);
        CHECK (r.playing (0) == 8 * ppq);
        r.runTo (9.0 * ppq);
        CHECK (r.playing (0) == -1); // stops after the last
    }
    {
        perfmode165::Run r (with (M::exclusiveRandom));
        r.player.seed (12345);
        r.command (T::press, 0, 0);
        auto last = (std::int64_t) 0;
        auto changes = 0;
        for (int k = 1; k <= 6; ++k)
        {
            r.runTo (k * 4.0 * ppq + 120.0);
            const auto now = r.playing (0);
            CHECK (now >= 0 && now != last); // never the same clip twice
            changes += now != last ? 1 : 0;
            last = now;
        }
        CHECK (changes == 6);
    }
}

TEST (perfmode165_press_modes)
{
    using namespace lynx;
    using T = perf::Command::Type;
    using P = TrackPerformance::Press;
    auto with = [] (P press, TrackPerformance::Motion motion = TrackPerformance::Motion::stay) {
        auto s = perfmode165::zoneSong();
        s.playlistTracks[0].performance.press = press;
        s.playlistTracks[0].performance.motion = motion;
        s.playlistTracks[0].performance.triggerSync = TrackPerformance::Sync::off;
        return s;
    };
    {
        perfmode165::Run r (with (P::latch));
        r.command (T::press, 0, 0);
        r.runTo (240.0);
        CHECK (r.playing (0) == 0);
        r.command (T::press, 0, 0); // a second press stops it
        r.runTo (480.0);
        CHECK (r.playing (0) == -1);
    }
    {
        perfmode165::Run r (with (P::retrigger));
        r.command (T::press, 0, 0);
        r.runTo (2.0 * ppq);
        r.command (T::press, 0, 0); // starts over
        r.runTo (2.0 * ppq + 120.0);
        CHECK (r.playing (0) == 0 && r.live.tracks[0].progress.load() < 0.05f);
    }
    {
        perfmode165::Run r (with (P::holdStop));
        r.command (T::press, 0, 0);
        r.runTo (2.0 * ppq);
        CHECK (r.playing (0) == 0);
        r.command (T::release, 0, 0);
        r.runTo (2.0 * ppq + 240.0);
        CHECK (r.playing (0) == -1);
    }
    {
        perfmode165::Run r (with (P::holdMotion, TrackPerformance::Motion::marchWrap));
        r.command (T::press, 0, 0);
        r.runTo (9.0 * ppq);
        CHECK (r.playing (0) == 0); // held: it loops
        r.command (T::release, 0, 0);
        r.runTo (12.0 * ppq + 120.0);
        CHECK (r.playing (0) == 4 * ppq); // let go: the motion goes on at the end of the pass
    }
}

TEST (perfmode165_position_sync_and_notes)
{
    using namespace lynx;
    using T = perf::Command::Type;
    auto s = perfmode165::zoneSong();
    s.playlistTracks[0].performance.triggerSync = TrackPerformance::Sync::beat;
    s.playlistTracks[0].performance.positionSync = TrackPerformance::Sync::fourBeats;
    s.playlistTracks[0].performance.tolerant = false;
    perfmode165::Run r (std::move (s));
    r.runTo (4.0 * ppq + 1.0 * ppq + 120.0); // launch during beat 2 of bar 2
    r.command (T::press, 0, 4 * ppq);
    r.runTo (6.0 * ppq + 120.0); // starts on beat 3, at its third beat (the song position modulo 4 beats)
    CHECK (r.playing (0) == 4 * ppq);
    CHECK (std::abs (r.live.tracks[0].progress.load() - 0.5f) < 0.01f); // (at the last block's start)

    // the launched clip's note sounds on the clip's first beat of each pass (bar 3)
    const auto tps = ticksPerSample (120.0, 48000.0);
    EventBuffer ev;
    auto clock = 6.0 * ppq + 120.0;
    std::vector<double> ons;
    while (clock < 12.0 * ppq)
    {
        const auto n = 512;
        r.player.process (r.song, r.zone, &r.live, clock, clock, n * tps, true);
        ev.clear();
        perf::schedulePerformance (r.song, r.player, clock, n, tps, ev);
        for (const auto& e : ev)
            if (e.type == NoteEvent::Type::on)
            {
                CHECK (e.key == 62);
                ons.push_back (clock + e.sampleOffset * tps);
            }
        clock += n * tps;
    }
    CHECK (ons.size() == 2 && std::abs (ons[0] - 8.0 * ppq) < 1.0 && std::abs (ons[1] - 12.0 * ppq) < 1.0); // (Stay: it loops)
}

TEST (perfmode165_stop_and_recording)
{
    using namespace lynx;
    using T = perf::Command::Type;
    auto s = perfmode165::zoneSong();
    s.playlistTracks[0].performance.triggerSync = TrackPerformance::Sync::off;
    perfmode165::Run r (std::move (s));
    r.live.recording.store (true);
    r.command (T::press, 0, 4 * ppq);
    r.runTo (6.0 * ppq);
    r.command (T::stopTrack, 0, 0);
    r.runTo (6.0 * ppq + 240.0);
    CHECK (r.playing (0) == -1);
    perf::RecordedPass pass;
    std::vector<perf::RecordedPass> passes;
    while (r.live.recorded.pop (pass))
        passes.push_back (pass);
    // a whole pass (0…4 beats) and the pass the stop cut short (4…6 beats)
    CHECK (passes.size() == 2);
    if (passes.size() == 2)
    {
        CHECK (passes[0].track == 0 && passes[0].clipStart == 4 * ppq);
        CHECK (std::abs (passes[0].songTick) < 1.0 && std::abs (passes[0].length - 4.0 * ppq) < 1.0);
        CHECK (std::abs (passes[1].songTick - 4.0 * ppq) < 1.0 && std::abs (passes[1].length - 2.0 * ppq) < 1.0);
        Clip c;
        CHECK (perf::recordedClip (r.song, passes[1], c));
        CHECK (c.pattern == 1 && c.length == 2 * ppq && c.start == 4 * ppq && c.offset == 0);
    }

    // a full pass of a looping clip is recorded at the end of each pass; stopping the transport records the rest
    perfmode165::Run r2 (perfmode165::zoneSong());
    r2.live.recording.store (true);
    r2.command (T::press, 0, 0);
    r2.runTo (12.0 * ppq);     // FL's default sync 4 beats: starts at 0, two passes ended at 4 and 8 beats
    r2.player.process (r2.song, r2.zone, &r2.live, r2.clock, r2.clock, 0.0, false); // transport stops
    passes.clear();
    while (r2.live.recorded.pop (pass))
        passes.push_back (pass);
    CHECK (passes.size() == 3);
    if (passes.size() == 3)
        CHECK (std::abs (passes[1].songTick - 4.0 * ppq) < 1.0 && std::abs (passes[2].length - 4.0 * ppq) < 1.0);
    CHECK (r2.live.anyPlaying.load() == false);
}

TEST (perfmode165_flp_import_track_settings)
{
    if (realFlpDir.empty())
        return;
    const auto path = std::filesystem::path (realFlpDir) / "Demo projects" / "Performance mode" / "Charles Deluxe - Performance Demo.flp";
    if (! std::filesystem::exists (path))
        return;
    std::ifstream in (path, std::ios::binary);
    const std::vector<std::uint8_t> file ((std::istreambuf_iterator<char> (in)), {});
    const auto song = lynx::importFlp (flp::parseProject (file));
    using lynx::TrackPerformance;
    CHECK (song.playlistTracks.size() >= 3);
    if (song.playlistTracks.size() >= 3)
    {
        // [DATA] track 1: Stay, Auto trigger sync; track 2: One shot, 1 beat, not tolerant; track 3: March & wrap
        CHECK (song.playlistTracks[0].performance.triggerSync == TrackPerformance::Sync::autoLength);
        CHECK (song.playlistTracks[1].performance.motion == TrackPerformance::Motion::oneShot);
        CHECK (song.playlistTracks[1].performance.triggerSync == TrackPerformance::Sync::beat);
        CHECK (! song.playlistTracks[1].performance.tolerant);
        CHECK (song.playlistTracks[2].performance.motion == TrackPerformance::Motion::marchWrap);
    }
    // the zone ends at the Start marker (bar 21)
    bool start = false;
    for (const auto& m : song.markers())
        start = start || (m.action == lynx::Marker::songStart && m.tick == 1920 * 10);
    CHECK (start);
    CHECK (song.performanceMode); // [DATA] event 0x1F = 1 in every performance demo
}

namespace perfmode165
{
// A synth channel on insert 1 with an effect chain (one effect at half dry/wet, one switched off), a short phrase in
// pattern mode; rendered for `blocks` × 512 samples at 48 kHz (left + right interleaved per block).
inline lynx::Song patcherizeSong()
{
    using namespace lynx;
    auto s = Song::makeDefault();
    s.mode = PlayMode::pattern;
    s.channels.resize (1);
    auto& ch = s.channels[0];
    ch.name = "Lead";
    ch.instrument = InstrumentKind::synth;
    ch.mixerTrack = 1;
    ch.volume = 0.6f;
    ch.pan = -0.3f;
    s.patterns.resize (1);
    for (int k = 0; k < 8; ++k)
    {
        Note n;
        n.start = k * ppq / 2;
        n.length = ppq / 3;
        n.key = 57 + (k * 5) % 12;
        n.velocity = 0.5f + 0.06f * (float) k;
        s.patterns[0].notes.push_back (n);
    }
    s.patterns[0].lengthTicks = 4 * ppq;
    s.currentPattern = 0;
    auto& fx = s.mixer[1].effects;
    fx.clear();
    for (auto kind : { EffectKind::eq, EffectKind::compressor, EffectKind::chorus, EffectKind::delay, EffectKind::reverb })
    {
        fx.push_back (makeEffectSlot (kind));
    }
    fx[2].mix = 0.5f;
    fx[3].enabled = false;
    fx[0].params[0] = 0.7f;
    return s;
}

inline std::vector<float> render (const lynx::Song& song, int blocks = 200)
{
    auto state = lynx::RackState::build (song, nullptr, 48000.0);
    lynx::RackRenderer r;
    r.prepare (48000.0, 512);
    std::vector<float> all, l (512), rr (512);
    double tick = 0.0;
    for (int b = 0; b < blocks; ++b, tick += 512.0 * song.bpm * lynx::ppq / 60.0 / 48000.0)
    {
        r.process (state.get(), true, tick, 512, l.data(), rr.data(), nullptr);
        all.insert (all.end(), l.begin(), l.end());
        all.insert (all.end(), rr.begin(), rr.end());
    }
    return all;
}

inline float maxDiff (const std::vector<float>& a, const std::vector<float>& b)
{
    float m = a.size() == b.size() ? 0.0f : 1.0f;
    for (std::size_t i = 0; i < std::min (a.size(), b.size()); ++i)
        m = std::max (m, std::abs (a[i] - b[i]));
    return m;
}
inline float peak (const std::vector<float>& a)
{
    float m = 0.0f;
    for (auto v : a)
        m = std::max (m, std::abs (v));
    return m;
}
} // namespace perfmode165

TEST (perfmode165_patcherize_channel_nulls)
{
    using namespace lynx;
    const auto song = perfmode165::patcherizeSong();
    const auto before = perfmode165::render (song);
    CHECK (perfmode165::peak (before) > 0.01f);
    auto patched = song;
    CHECK (patcherize::canPatcherizeChannel (patched, 0));
    CHECK (patcherize::patcherizeChannel (patched, 0));
    const auto& ch = patched.channels[0];
    CHECK (ch.instrument == InstrumentKind::patcher && ch.patcher.nodes.size() == 1 && ch.patcher.nodes[0].generator);
    CHECK (ch.patcher.nodes[0].channel.instrument == InstrumentKind::synth);
    CHECK (ch.volume == 0.6f && ch.pan == -0.3f && ch.mixerTrack == 1); // the channel settings stay outside
    CHECK (! patcherize::canPatcherizeChannel (patched, 0));          // (already a Patcher)
    const auto after = perfmode165::render (patched);
    const auto d = perfmode165::maxDiff (before, after);
    std::printf ("  patcherize channel: peak %.4f, max diff %.3g\n", perfmode165::peak (before), d);
    CHECK (d < 1.0e-6f);
}

TEST (perfmode165_patcherize_slots_null_and_follow_automation)
{
    using namespace lynx;
    auto song = perfmode165::patcherizeSong();
    // an automation of the chorus's first parameter and a controller link to the reverb's
    Automation a;
    a.target = { AutomationTargetKind::effectParam, 1, 2, 0 };
    song.automations.push_back (a);
    MidiLink link;
    link.cc = 20;
    link.target = { AutomationTargetKind::effectParam, 1, 4, 1 };
    song.midiLinks.push_back (link);
    const auto before = perfmode165::render (song);

    auto one = song;
    CHECK (patcherize::patcherizeSlots (one, 1, 2, false) == 2);
    CHECK (one.mixer[1].effects.size() == 5 && one.mixer[1].effects[2].kind == EffectKind::patcher);
    CHECK (one.mixer[1].effects[2].patcher.nodes.size() == 1 && one.mixer[1].effects[2].patcher.nodes[0].effect.mix == 0.5f);
    CHECK (one.automations[0].target.slot == 2 && one.automations[0].target.param == 0
           && one.mixer[1].effects[2].patcher.published.size() == 1);
    const auto d1 = perfmode165::maxDiff (before, perfmode165::render (one));

    auto all = song;
    CHECK (patcherize::slotsForAll (all, 1).size() == 5);
    CHECK (patcherize::patcherizeSlots (all, 1, 0, true) == 0);
    const auto& fx = all.mixer[1].effects;
    CHECK (fx.size() == 1 && fx[0].kind == EffectKind::patcher && fx[0].patcher.nodes.size() == 5);
    CHECK (fx[0].patcher.nodes[3].effect.enabled == false);
    // the chorus automation → published 0 (node 2, param 0); the reverb link → published 1 (node 4, param 1)
    CHECK (all.automations[0].target.slot == 0 && all.midiLinks[0].target.slot == 0);
    if (fx[0].patcher.published.size() == 2)
    {
        const auto& p = fx[0].patcher.published;
        CHECK (p[(std::size_t) all.automations[0].target.param].node == 2 && p[(std::size_t) all.automations[0].target.param].param == 0);
        CHECK (p[(std::size_t) all.midiLinks[0].target.param].node == 4 && p[(std::size_t) all.midiLinks[0].target.param].param == 1);
    }
    else
        CHECK (false);
    const auto d2 = perfmode165::maxDiff (before, perfmode165::render (all));
    std::printf ("  patcherize slot: max diff %.3g, all: %.3g\n", d1, d2);
    CHECK (d1 < 1.0e-6f);
    CHECK (d2 < 1.0e-6f);
}

TEST (perfmode165_midi_channel_launches_clips)
{
    using namespace lynx;
    // [DECOMP] 0x1181be1: an octave per track, a note per column; 120…124 navigate
    perf::MidiMapping m;
    perf::Command c;
    CHECK (m.note (13, true, c) && c.type == perf::Command::Type::press && c.track == 1 && c.column == 1);
    CHECK (m.note (13, false, c) && c.type == perf::Command::Type::release);
    CHECK (! m.note (121, true, c) && m.trackOffset == 1);
    CHECK (! m.note (124, true, c) && m.bigSteps);
    CHECK (! m.note (123, true, c) && m.columnOffset == 12);
    CHECK (! m.note (120, true, c) && m.trackOffset == 0);
    CHECK (m.note (2, true, c) && c.track == 0 && c.column == 14);

    // a controller's column resolves to the track's n-th zone clip
    perfmode165::Run r (perfmode165::zoneSong());
    m = {};
    CHECK (m.note (2, true, c));
    r.live.midiCommands.push (c);
    r.runTo (4.0 * ppq + 120.0);
    CHECK (r.playing (0) == 8 * ppq);

    prefs::MidiPrefs p;
    p.performanceChannel = 16;
    CHECK (prefs::MidiPrefs::fromText (p.toText()).performanceChannel == 16);
    CHECK (prefs::MidiPrefs::fromText (prefs::MidiPrefs {}.toText()).performanceChannel == 0);
}
