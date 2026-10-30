// Audit round 165z-a (issues #2–#9 against FL 2026, docs/analise/REA_rodada165z-a.md)

TEST (audit165za_song_mode_loops_at_the_exact_end_of_the_last_clip)
{
    // FL 2026 measured: a clip 2:05–3:05 (4/4) loops at 3:05, not at the end of bar 3
    auto s = lynx::Song::makeDefault();
    const auto bar = (std::int64_t) lynx::ppq * s.beatsPerBar;
    s.mode = lynx::PlayMode::song;
    s.playlist = { lynx::Clip { 0, 0, bar + 2 * lynx::ppq, bar, 0 } };
    CHECK (s.songLength() == 2 * bar + 2 * lynx::ppq);
    const auto loop = lynx::playbackLoop (true, 0.0, 0.0, (double) s.songLength());
    CHECK (loop.active() && loop.end == (double) (2 * bar + 2 * lynx::ppq));

    // a clip ending on a bar line: unchanged
    s.playlist = { lynx::Clip { 0, 0, 0, 3 * bar, 0 } };
    CHECK (s.songLength() == 3 * bar);

    // empty Playlist: the current pattern's length (at least a bar), as before
    s.playlist.clear();
    s.patterns[0].notes.clear();
    CHECK (s.songLength() == bar);

    // export renders to the same end (no rounding there either)
    s.playlist = { lynx::Clip { 0, 0, bar + 2 * lynx::ppq, bar, 0 } };
    std::int64_t length = 0;
    lynx::makeExportSong (s, true, 0, length);
    CHECK (length == s.songLength());
}

TEST (audit165za_generator_dropped_on_a_channel_replaces_it)
{
    // FL 2026 measured: 3x Osc dragged from the Plugin database onto "808 Snare" (Insert 4) turns that channel into
    // "3x Osc" on Insert 4; dropped below the channels it is a new channel
    auto s = lynx::Song::makeDefault();
    while (s.channels.size() < 2)
        s.channels.emplace_back();
    auto& target = s.channels[1];
    target.name = "808 Snare";
    target.instrument = lynx::InstrumentKind::sampler;
    target.samplePath = "snare.wav";
    target.mixerTrack = 4;
    target.volume = 0.5f;
    target.pan = -0.25f;
    target.muted = true;
    target.colour = 0xFF112233u;
    target.group = 0;
    target.swingMix = 0.4f;
    lynx::Note n;
    n.channel = 1;
    s.patterns[0].notes.push_back (n);
    const auto notes = s.patterns[0].notes.size();
    const auto channels = s.channels.size();

    lynx::ChannelDef osc;
    osc.name = "3x Osc";
    osc.instrument = lynx::InstrumentKind::synth;
    osc.mixerTrack = 9;
    CHECK (lynx::replaceGenerator (s, 1, osc));
    const auto& c = s.channels[1];
    CHECK (s.channels.size() == channels);
    CHECK (c.name == "3x Osc" && c.instrument == lynx::InstrumentKind::synth && c.samplePath.empty());
    CHECK (c.mixerTrack == 4 && c.volume == 0.5f && c.pan == -0.25f && c.muted && c.colour == 0xFF112233u && c.group == 0
           && c.swingMix == 0.4f);
    CHECK (s.patterns[0].notes.size() == notes); // its notes stay with it

    CHECK (! lynx::replaceGenerator (s, (int) s.channels.size(), osc));
    CHECK (! lynx::replaceGenerator (s, -1, osc));
}
