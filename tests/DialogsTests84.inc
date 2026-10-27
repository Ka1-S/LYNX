// --- Export options, MP3 encoder, plugin performance monitor, clip properties / arrangements, new project, touch
// keyboard (core/ExportOptions.h, core/Mp3Encoder.h, core/PluginMonitor.h, core/PlaylistOps.h, core/Templates.h,
// core/TouchKeyboard.h) - round 84 ------------------------------------------------------------------------------

TEST (export_options_text_round_trip_and_clamping)
{
    using namespace lynx::exportopts;
    Options o;
    o.format = Format::mp3;
    o.mp3Kbps = 192;
    o.oggKbps = 160;
    o.flacLevel = 8;
    o.flacBits = 16;
    o.wavBits = 32;
    o.sampleRate = 96000.0;
    o.dither = DitherMode::tpdf;
    o.tail = TailMode::wrap;
    o.maxTailSeconds = 4.5;
    o.range = Range::selection;
    o.loops = 3;
    o.stems = Stems::channels;
    o.stemsIncludeMaster = true;
    o.normalize = true;
    o.masterEffects = false;
    o.trimPdc = false;
    o.channels = Channels::monoMerged;
    CHECK (fromText (toText (o)) == o);
    CHECK (fromText ("") == Options {});

    const auto bad = fromText ("format=9\nmp3Kbps=300\nsampleRate=50000\nloops=99\nwavBits=20\nflacLevel=-3\nmaxTail=nan\ntail=7\nchannels=x\n");
    CHECK (bad.format == Options {}.format && bad.mp3Kbps == 320 && bad.sampleRate == 48000.0 && bad.loops == 16);
    CHECK (bad.wavBits == 24 && bad.flacLevel == 0 && bad.tail == TailMode::leave && bad.channels == Channels::stereo);
    CHECK (bad.maxTailSeconds == Options {}.maxTailSeconds);

    CHECK (std::string (extension (Format::mp3)) == ".mp3" && std::string (extension (Format::flac)) == ".flac");
    Options w;
    w.wavBits = 16;
    CHECK (usesDither (w) && fileBits (w) == 16);
    w.wavBits = 32;
    CHECK (! usesDither (w) && fileBits (w) == 32);
    w.wavBits = 24;
    w.dither = DitherMode::none;
    CHECK (! usesDither (w));
    w.format = Format::ogg;
    w.dither = DitherMode::shaped;
    CHECK (! usesDither (w) && fileBits (w) == 16);
    CHECK (mp3SampleRate (44100.0) == 44100.0 && mp3SampleRate (96000.0) == 48000.0 && mp3SampleRate (88200.0) == 44100.0);
    CHECK (formatDuration (65.4) == "1:05" && formatDuration (3725.0) == "1:02:05" && formatDuration (-3.0) == "0:00");
}

TEST (export_render_song_ranges_loops_and_tail)
{
    using namespace lynx;
    using namespace lynx::exportopts;
    auto song = Song::makeDefault();
    const auto bar = (std::int64_t) ppq * song.beatsPerBar;
    Clip a;
    a.pattern = 0;
    a.start = 0;
    a.length = 2 * bar;
    Clip b = a;
    b.start = 3 * bar;
    b.length = 2 * bar;
    b.track = 1;
    song.playlist = { a, b };
    song.loopStart = bar;      // selection: bars 2..4
    song.loopEnd = 4 * bar;

    Options o;
    std::int64_t music = 0, loop = 0;
    auto whole = makeRenderSong (song, o, 0, music, loop);
    CHECK (music == 5 * bar && loop == 5 * bar && whole.mode == PlayMode::song && whole.loopEnd == 0 && whole.playlist.size() == 2);

    o.range = Range::selection;
    auto sel = makeRenderSong (song, o, 0, music, loop);
    CHECK (music == 3 * bar && loop == 3 * bar);
    CHECK (sel.playlist.size() == 2);
    if (sel.playlist.size() == 2)
    {
        // clip a cut at the selection's start (its second bar, offset by one bar); clip b cut at its end
        CHECK (sel.playlist[0].start == 0 && sel.playlist[0].length == bar && sel.playlist[0].offset == bar);
        CHECK (sel.playlist[1].start == 2 * bar && sel.playlist[1].length == bar && sel.playlist[1].offset == 0);
    }

    o.loops = 3;
    o.masterEffects = false;
    auto looped = makeRenderSong (song, o, 2 * bar, music, loop);
    CHECK (loop == 3 * bar && music == 9 * bar);
    CHECK (looped.playlist.size() == 7); // 2 clips x 3 passes + the tail
    CHECK (looped.playlist.back().start == 9 * bar && looped.patterns.back().name == "(tail)");
    CHECK (looped.mixer[0].effects.empty() && ! song.mixer[0].effects.empty());

    o = {};
    o.range = Range::pattern;
    auto pat = makeRenderSong (song, o, 0, music, loop);
    CHECK (pat.playlist.size() == 1 && music == song.patterns[0].effectiveLength (song.beatsPerBar));

    // an empty selection still renders its length (silence)
    song.playlist.clear();
    o.range = Range::selection;
    makeRenderSong (song, o, 0, music, loop);
    CHECK (music == 3 * bar);
    song.loopEnd = 0;
    CHECK (! hasSelection (song));
}

TEST (export_tail_wrap_cut_normalize_mono)
{
    using namespace lynx::exportopts;
    std::vector<float> l { 1, 2, 3, 4, 10, 20, 30 }, r { 0, 0, 0, 0, 1, 1, 1 };
    auto l2 = l, r2 = r;
    applyTail (l2, r2, 4, TailMode::wrap);
    CHECK ((l2 == std::vector<float> { 11, 22, 33, 4 }) && (r2 == std::vector<float> { 1, 1, 1, 0 }));
    l2 = l, r2 = r;
    applyTail (l2, r2, 4, TailMode::cut);
    CHECK ((l2 == std::vector<float> { 1, 2, 3, 4 }) && r2.size() == 4);
    l2 = l, r2 = r;
    applyTail (l2, r2, 4, TailMode::leave);
    CHECK (l2 == l);
    l2 = l, r2 = r; // a tail longer than the loop wraps around more than once
    applyTail (l2, r2, 2, TailMode::wrap);
    CHECK ((l2 == std::vector<float> { 1 + 3 + 10 + 30, 2 + 4 + 20 }));
    l2 = { 1, 2 }, r2 = { 0, 0 }; // a render shorter than the loop is padded with silence
    applyTail (l2, r2, 4, TailMode::cut);
    CHECK ((l2 == std::vector<float> { 1, 2, 0, 0 }) && r2.size() == 4);

    CHECK (normalizeGain (0.5f) == 2.0f && normalizeGain (0.0f) == 1.0f);
    float a[2] { 0.2f, 0.4f };
    const float b[2] { 0.6f, -0.4f };
    toChannels (a, b, 2, Channels::monoMerged);
    CHECK (std::abs (a[0] - 0.4f) < 1e-6f && std::abs (a[1]) < 1e-6f);
    toChannels (a, b, 2, Channels::monoRight);
    CHECK (a[0] == 0.6f && a[1] == -0.4f);
    CHECK (channelCount (Channels::stereo) == 2 && channelCount (Channels::monoLeft) == 1);
}

TEST (export_stems_tracks_channels_and_names)
{
    using namespace lynx;
    using namespace lynx::exportopts;
    auto song = Song::makeDefault(); // channels 0..4 on inserts 1..5, notes in pattern 0
    song.setRoute (5, 7, true, 0.5f);
    const auto tracks = stemTracks (song);
    CHECK ((tracks == std::vector<int> { 1, 2, 3, 4, 5, 7 }));

    Clip c;
    c.pattern = 0;
    c.length = ppq * 4;
    song.playlist = { c };
    const auto channels = stemChannels (song, Range::song);
    std::set<int> withNotes;
    for (const auto& n : song.patterns[0].notes)
        withNotes.insert (n.channel);
    CHECK (channels == std::vector<int> (withNotes.begin(), withNotes.end()));
    song.playlist[0].muted = true;
    CHECK (stemChannels (song, Range::song).empty());
    CHECK (stemChannels (song, Range::pattern).size() == withNotes.size());

    const auto solo = makeChannelStemSong (song, 1);
    CHECK (! solo.channels[1].muted && solo.channels[0].muted && solo.channels[4].muted);
    CHECK (stemFileName ("Night", 3, "Lead: \"hi\"/2", ".wav") == "Night - 3 Lead_ _hi__2.wav");
    CHECK (stemFileName ("Night", 1, " ..", ".mp3") == "Night - 1 .mp3" || stemFileName ("Night", 1, "", ".mp3") == "Night - 1.mp3");
}

TEST (export_dither_without_noise_shaping)
{
    lynx::Dither shaped (16), flat (16, false);
    std::vector<float> a (4096), b (4096);
    for (std::size_t i = 0; i < a.size(); ++i)
        a[i] = b[i] = 0.25f * (float) std::sin ((double) i * 0.05);
    shaped.process (a.data(), (int) a.size(), 0);
    flat.process (b.data(), (int) b.size(), 0);
    double e = 0.0;
    for (std::size_t i = 0; i < b.size(); ++i)
        e = std::max (e, std::abs ((double) b[i] - flat.writerBias() - 0.25 * std::sin ((double) i * 0.05)));
    CHECK (e <= 1.5 / 32768.0); // plain TPDF: at most 1.5 LSB away
    CHECK (a != b);
    // both land on the 16-bit grid
    const auto steps = (b[100] - flat.writerBias()) * 32768.0f;
    CHECK (std::abs (steps - std::round (steps)) < 1e-3f);
}

TEST (export_mp3_encoder_frames)
{
    for (const auto [rate, kbps, channels] : { std::tuple { 44100, 320, 2 }, std::tuple { 48000, 128, 1 } })
    {
        lynx::Mp3Encoder enc;
        std::string error;
        CHECK (enc.open (rate, rate, channels, kbps, error, "Night Drive", "LYNX"));
        std::vector<float> l ((std::size_t) rate), r ((std::size_t) rate);
        for (std::size_t i = 0; i < l.size(); ++i)
        {
            l[i] = 0.5f * (float) std::sin (6.283185307 * 440.0 * (double) i / rate);
            r[i] = 0.5f * (float) std::sin (6.283185307 * 660.0 * (double) i / rate);
        }
        std::vector<std::uint8_t> bytes;
        for (std::size_t done = 0; done < l.size(); done += 4096)
            CHECK (enc.encode (l.data() + done, r.data() + done, (int) std::min<std::size_t> (4096, l.size() - done), bytes));
        CHECK (enc.finish (bytes));
        CHECK (bytes.size() > 3 && bytes[0] == 'I' && bytes[1] == 'D' && bytes[2] == '3');
        const auto scan = lynx::scanMp3 (bytes);
        // one second = rate / 1152 frames (plus the encoder's delay and padding frames)
        CHECK (scan.frames >= rate / 1152 && scan.frames <= rate / 1152 + 4);
        CHECK (scan.kbps == kbps && scan.sampleRate == rate && scan.channels == channels);
    }

    lynx::Mp3Encoder resampled; // 96 kHz in, 48 kHz out
    std::string error;
    CHECK (resampled.open (96000, 48000, 2, 256, error));
    std::vector<float> silence (96000, 0.0f);
    std::vector<std::uint8_t> bytes;
    resampled.encode (silence.data(), silence.data(), (int) silence.size(), bytes);
    resampled.finish (bytes);
    const auto scan = lynx::scanMp3 (bytes);
    CHECK (scan.sampleRate == 48000 && scan.frames >= 41 && scan.frames <= 46);
}

TEST (perfmon_sources_loads_and_find)
{
    using namespace lynx;
    using namespace lynx::perfmon;
    auto song = Song::makeDefault();
    song.channels[4].instrument = InstrumentKind::plugin;
    song.channels[4].plugin.name = "Serum";
    EffectSlot fx;
    fx.kind = EffectKind::plugin;
    fx.plugin.name = "Pro-Q";
    song.mixer[3].effects.push_back (fx);
    const auto list = sources (song, [] (const ChannelDef& c) { return "Built-in " + c.name; });
    CHECK (list.size() == song.channels.size() + 2); // + the Master limiter and Pro-Q
    CHECK (list[0].name == "Built-in Kick" && list[0].channel == 0 && ! list[0].plugin);
    CHECK (list[4].name == "Serum" && list[4].plugin);
    const auto& proq = list.back();
    CHECK (proq.name == "Pro-Q" && proq.track == 3 && proq.slot == 0 && proq.key == effectKey (3, 0) && proq.plugin);

    Monitor m;
    std::vector<std::uint64_t> busy (keyCount, 0);
    m.update (list, busy, 0, 0); // starting point
    busy[4] = 50'000'000;        // Serum: 50 ms for 100 ms of audio = 50 %
    busy[(std::size_t) effectKey (3, 0)] = 10'000'000;
    m.update (list, busy, 100'000'000, 70'000'000);
    auto rows = m.rows();
    CHECK (rows.size() == list.size() && rows[0].source.name == "Serum");
    CHECK (std::abs (rows[0].current - 25.0) < 1e-9 && std::abs (rows[0].peak - 50.0) < 1e-9 && std::abs (rows[0].average - 50.0) < 1e-9);
    CHECK (std::abs (m.rackPeak() - 70.0) < 1e-9);

    busy[4] += 50'000'000;
    m.update (list, busy, 200'000'000, 140'000'000);
    rows = m.rows ("serum");
    CHECK (rows.size() == 1 && std::abs (rows[0].current - 37.5) < 1e-9 && std::abs (rows[0].average - 50.0) < 1e-9);
    CHECK (m.rows ("insert 3 pro").size() == 1 && m.rows ("nothing like this").empty());

    m.reset();
    busy[4] += 10'000'000;
    m.update (list, busy, 300'000'000, 150'000'000);
    rows = m.rows ("serum");
    CHECK (rows.size() == 1 && std::abs (rows[0].peak - 10.0) < 1e-9 && std::abs (rows[0].average - 10.0) < 1e-9);

    // something else in the same slot starts with a fresh history
    auto other = list;
    other[4].name = "Vital";
    busy[4] += 90'000'000;
    m.update (other, busy, 400'000'000, 160'000'000);
    rows = m.rows ("vital");
    CHECK (rows.size() == 1 && rows[0].peak == 0.0 && rows[0].current == 0.0);
    CHECK (m.rows ({}, SortBy::name).front().source.name < m.rows ({}, SortBy::name).back().source.name);
}

TEST (clipprops_read_apply_and_groups)
{
    using namespace lynx;
    using namespace lynx::clipprops;
    auto song = Song::makeDefault();
    song.patterns.emplace_back().name = "Drop";
    ChannelDef audio;
    audio.name = "Vox";
    audio.instrument = InstrumentKind::audioClip;
    song.channels.push_back (audio);
    const auto vox = (int) song.channels.size() - 1;
    Clip p;
    p.pattern = 0;
    p.length = ppq * 4;
    Clip a;
    a.kind = ClipKind::audio;
    a.channel = vox;
    a.start = ppq * 8;
    a.length = ppq * 4;
    song.playlist = { p, a };

    Props props;
    CHECK (read (song, 0, props) && props.kind == ClipKind::pattern && props.name == song.patterns[0].name && props.source == 0);
    CHECK (! read (song, 5, props));
    props.source = 1;
    props.name = "Drop B";
    props.colour = 0xff336699u;
    props.start = -50;
    props.length = 0;
    props.offset = ppq;
    CHECK (apply (song, 0, props));
    const auto& c0 = song.playlist[0];
    CHECK (c0.pattern == 1 && c0.start == 0 && c0.length == 1 && c0.offset == ppq);
    CHECK (song.patterns[1].name == "Drop B" && song.patterns[1].colour == 0xff336699u);

    CHECK (read (song, 1, props) && props.kind == ClipKind::audio && props.name == "Vox");
    props.gain = 9.0f;
    props.fadeIn = ppq * 10;
    props.fadeOut = ppq * 10;
    props.pitch = 40.0f;
    props.stretchBeats = 0.0f;
    props.keepPitch = true;
    props.source = 0; // channel 0 is not an audio clip channel: ignored
    CHECK (apply (song, 1, props));
    const auto& c1 = song.playlist[1];
    CHECK (c1.channel == vox && c1.gain == 4.0f && c1.fadeIn == c1.length && c1.fadeOut == 0);
    // (round 163: "keep length" stays without a stretch — it keeps a pitched clip's length)
    CHECK (song.channels[(std::size_t) vox].sampler.pitch == 24.0f && song.channels[(std::size_t) vox].sampler.keepPitch);

    changeMute (song, { 0, 1 }, MuteChange::muteAll);
    CHECK (song.playlist[0].muted && song.playlist[1].muted);
    song.playlist[0].muted = false;
    changeMute (song, { 0, 1, 7 }, MuteChange::flip);
    CHECK (song.playlist[0].muted && ! song.playlist[1].muted);
    changeGain (song, { 0, 1 }, 0.5f, true);
    CHECK (song.playlist[1].gain == 2.0f && song.playlist[0].gain == 1.0f); // pattern clips have no gain
    changeGain (song, { 1 }, 0.25f, false);
    CHECK (song.playlist[1].gain == 0.25f);

    CHECK (sourcesFor (song, ClipKind::audio).size() == 1 && sourcesFor (song, ClipKind::audio)[0].first == vox);
    CHECK (sourcesFor (song, ClipKind::pattern).size() == 2);
}

TEST (arrangements_merge_insert_replace_move)
{
    using namespace lynx;
    using namespace lynx::arrangeops;
    Song song;
    song.beatsPerBar = 4;
    const auto bar = (std::int64_t) ppq * 4;
    auto clip = [] (std::int64_t start, std::int64_t length, int track) {
        Clip c;
        c.start = start;
        c.length = length;
        c.track = track;
        return c;
    };
    song.playlist = { clip (0, 4 * bar, 0), clip (4 * bar, 2 * bar, 1) };       // current: 6 bars
    song.addMarker ({ 5 * bar, "Chorus" });
    song.arrangements.push_back ({ "Intro", { clip (0, bar, 2), clip (bar, bar, 3) }, 0, 0, {} }); // 2 bars

    CHECK (lengthOf (song, 0) == 6 * bar && lengthOf (song, 1) == 2 * bar);
    CHECK (positionTick (song, MergePosition::end, 0) == 6 * bar && positionTick (song, MergePosition::current, 77) == 77);
    CHECK (! merge (song, 0, MergeMode::merge, 0));

    auto ins = song;
    CHECK (merge (ins, 1, MergeMode::insert, 2 * bar));
    // the 4-bar clip is split at bar 2: its second half moves after the 2 inserted bars
    CHECK (ins.playlist.size() == 5);
    CHECK (lengthOf (ins, 0) == 8 * bar);
    CHECK (ins.markers()[0].tick == 7 * bar);
    int at2 = 0;
    for (const auto& c : ins.playlist)
        if (c.start == 2 * bar && (c.track == 2))
            ++at2;
    CHECK (at2 == 1);

    auto mer = song;
    CHECK (merge (mer, 1, MergeMode::merge, 0));
    CHECK (mer.playlist.size() == 4 && lengthOf (mer, 0) == 6 * bar);

    auto rep = song;
    CHECK (merge (rep, 1, MergeMode::replace, 3 * bar));
    // [3, 5) bars cleared: the first clip keeps 3 bars, the second keeps its last bar (from bar 5)
    std::int64_t covered = 0;
    for (const auto& c : rep.playlist)
        if (c.track < 2)
            covered += c.length;
    CHECK (covered == 4 * bar && rep.playlist.size() == 4);

    std::vector<Clip> list { clip (0, 10 * bar, 0) };
    clearRange (list, 2 * bar, 3 * bar);
    CHECK (list.size() == 2 && list[0].length == 2 * bar && list[1].start == 3 * bar && list[1].offset == 3 * bar);

    song.arrangements.push_back ({ "Outro", {}, 0, 0, {} });
    song.selectArrangement (2);
    move (song, 2, 0);
    CHECK (song.currentArrangement == 0 && song.arrangements[0].name == "Outro");
    move (song, 1, 2);
    CHECK (song.currentArrangement == 0 && song.arrangements[2].name == "Arrangement 1");
    rename (song, 1, "Verse");
    rename (song, 1, "");
    CHECK (song.arrangements[1].name == "Verse");
    const auto copy = duplicate (song, 2);
    CHECK (copy == 3 && song.arrangements[3].name == "Arrangement 1 (copy)" && song.arrangements[3].clips.size() == 2);
}

TEST (new_project_setup_and_templates)
{
    using namespace lynx;
    CHECK (legalProjectName ("  My: song?. ") == "My_ song_" && legalProjectName (" .. ") == "Untitled");
    NewProjectSetup s;
    s.name = "Night Drive";
    CHECK (newProjectPath (s, ".lynx").empty());
    s.folder = "C:\\Music\\";
    CHECK (newProjectPath (s, ".lynx") == "C:\\Music/Night Drive/Night Drive.lynx");
    s.ownFolder = false;
    s.folder = "/home/x";
    CHECK (newProjectPath (s, ".lynx") == "/home/x/Night Drive.lynx");
    s.bpm = 900.0;
    s.beatsPerBar = 7;
    s.info.author = "Me";
    auto song = makeTemplate (0);
    applySetup (song, s);
    CHECK (song.bpm == 522.0 && song.beatsPerBar == 7 && song.info.author == "Me" && song.info.title == "Night Drive");
    CHECK (templateDisplayName ("C:/T/Trap kit.lynx") == "Trap kit" && templateDisplayName (".hidden") == ".hidden");
}

TEST (touch_keyboard_geometry_velocity_and_no_stuck_notes)
{
    using namespace lynx::touchkb;
    Layout k;
    k.lowest = 48;
    k.whiteKeys = 14;
    k.width = 140.0f;
    k.height = 100.0f;
    CHECK (k.highest() == 71);
    CHECK (k.noteAt (5.0f, 90.0f) == 48);      // C, bottom
    CHECK (k.noteAt (10.0f, 30.0f) == 49);     // C# over the C/D line
    CHECK (k.noteAt (10.0f, 80.0f) == 50 || k.noteAt (10.0f, 80.0f) == 48); // below the black key: a white key
    CHECK (k.noteAt (15.0f, 80.0f) == 50);     // D
    CHECK (k.noteAt (35.0f, 20.0f) == 53);     // F: no black key between E and F
    CHECK (k.noteAt (139.0f, 90.0f) == 71 && k.noteAt (141.0f, 50.0f) == -1 && k.noteAt (5.0f, -1.0f) == -1);
    CHECK (isBlack (61) && ! isBlack (60) && isBlack (-2) == isBlack (10));
    CHECK (std::abs (k.velocityAt (48, 0.0f) - 0.2f) < 1e-6f && std::abs (k.velocityAt (48, 100.0f) - 1.0f) < 1e-6f);
    CHECK (k.velocityAt (49, 61.0f) > 0.99f);  // black keys reach full velocity at their own bottom
    CHECK (k.velocityAt (48, 50.0f) > k.velocityAt (48, 20.0f));

    CHECK (shiftOctave (48, 1, 21) == 60 && shiftOctave (48, -10, 21) == 0 && shiftOctave (100, 5, 21) <= 127 - 35);
    CHECK (shiftOctave (50, 0, 21) == 48);

    // gestures through HeldNotes: press, glide to another key, release, leave: every on gets its off
    std::map<std::pair<int, int>, int> balance;
    int ons = 0;
    {
        lynx::HeldNotes notes ([&] (int channel, int key, float, bool on) {
            balance[{ channel, key }] += on ? 1 : -1;
            ons += on ? 1 : 0;
        });
        const int pointer = 0;
        notes.press (2, k.noteAt (5.0f, 90.0f), 0.8f, pointer);
        notes.releaseTag (pointer); // glide: the old key stops ...
        notes.press (2, k.noteAt (15.0f, 80.0f), 0.8f, pointer); // ... the new one starts
        notes.press (2, k.noteAt (35.0f, 20.0f), 0.5f, 1); // a second finger
        notes.releaseTag (pointer);
        // the mouse leaves with the second finger still down: releaseAll; then the window closes (destructor)
        notes.press (3, 60, 1.0f, 2);
    }
    CHECK (ons == 4);
    CHECK (std::all_of (balance.begin(), balance.end(), [] (const auto& e) { return e.second == 0; }));
}

TEST (clipprops_bar_beat_tick_text)
{
    using namespace lynx;
    using namespace lynx::clipprops;
    const auto beat = (std::int64_t) ppq, bar = beat * 4;
    CHECK (formatPosition (0, 4) == "1:1:000" && formatLength (0, 4) == "0:0:000");
    CHECK (formatPosition (2 * bar + beat + 5, 4) == "3:2:005" && formatLength (2 * bar + beat + 5, 4) == "2:1:005");
    CHECK (parsePosition ("3:2:005", 4) == 2 * bar + beat + 5 && parseLength ("2:1:005", 4) == 2 * bar + beat + 5);
    CHECK (parsePosition ("5", 4) == 4 * bar && parsePosition ("5:3", 4) == 4 * bar + 2 * beat && parseLength ("2", 4) == 2 * bar);
    CHECK (parsePosition ("0:1:000", 4) == -1 && parsePosition ("1:5:000", 4) == -1 && parsePosition ("1:1:x", 4) == -1 && parsePosition ("", 4) == -1);
    CHECK (parsePosition ("1:4", 3) == -1 && parsePosition ("2:3", 3) == 3 * beat + 2 * beat);
    for (std::int64_t t : { (std::int64_t) 0, beat * 7 + 33, bar * 100 + 1 })
        CHECK (parsePosition (formatPosition (t, 4), 4) == t && parseLength (formatLength (t, 7), 7) == t);
}
