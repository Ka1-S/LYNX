// Round 165p: issue #12 (BURNER1PA, Windows 10, 0.1.18)

// Bug: Metronome given 'C' did nothing — the focused Playlist / Piano Roll took 'C' for its Knife first. Now the
// editors' tools and letters are key map actions, and a key the user chose wins over the panels' built-in keys.
TEST (issue12_user_key_wins_over_a_panel_default)
{
    using namespace lynx::keys;
    KeyMap map;
    CHECK (map.conflicts().empty());
    CHECK (map.find ({ 'C' }, Context::playlist, false) == "playlist.tool.knife");
    CHECK (map.find ({ 'C' }, Context::pianoRoll, false) == "pianoRoll.tool.knife");
    CHECK (map.find ({ 'S' }, Context::playlist, false) == "playlist.tool.slip");
    CHECK (map.find ({ 'S' }, Context::pianoRoll, false) == "pianoRoll.slide");
    CHECK (map.find ({ 'N' }, Context::pianoRoll, false) == "pianoRoll.tool.drumPaint");
    CHECK (map.find ({ 'N' }, Context::playlist, false).empty()); // (no drum brush in the Playlist)
    CHECK (map.find ({ 'C' }, Context::playlist, true).empty());  // typing piano on: the letter plays a note
    CHECK (map.findUserBound ({ 'C' }, Context::playlist, false).empty()); // nothing chosen by the user yet

    map.setChords ("transport.metronome", { { 'C' } });
    for (auto focus : { Context::global, Context::channelRack, Context::pianoRoll, Context::playlist, Context::mixer, Context::browser })
        CHECK (map.findUserBound ({ 'C' }, focus, false) == "transport.metronome");
    CHECK (map.findUserBound ({ 'C' }, Context::playlist, true).empty()); // the typing piano still owns the letter
    CHECK (map.findUserBound ({ 'M', ctrl }, Context::global, false).empty()); // the old default key is gone
    const auto shadowed = map.shadowedBy ({ 'C' }, "transport.metronome");
    CHECK (shadowed.size() == 2);

    // a tool moved to another key works there, and its old letter is free
    map.setChords ("playlist.tool.knife", { { 'X' } });
    CHECK (map.findUserBound ({ 'X' }, Context::playlist, false) == "playlist.tool.knife");
    CHECK (map.findUserBound ({ 'X' }, Context::pianoRoll, false).empty());
    // both the user's: the focused panel's wins in the panel
    map.setChords ("pianoRoll.tool.listen", { { 'C' } });
    CHECK (map.findUserBound ({ 'C' }, Context::pianoRoll, false) == "pianoRoll.tool.listen");
    CHECK (map.findUserBound ({ 'C' }, Context::playlist, false) == "transport.metronome");
}

// Every action of the key map answers a key the user gives it, in every place it works (no panel swallows it).
TEST (issue12_every_action_takes_a_user_key)
{
    using namespace lynx::keys;
    const Chord picks[] = { { 'C' }, { 'Q', ctrl | shift }, { f3, alt }, { 'K', ctrl } };
    for (const auto& a : actions())
        for (const auto& c : picks)
        {
            KeyMap map;
            map.setChords (a.id, { c });
            const auto inDefaults = std::find (a.defaults.begin(), a.defaults.end(), c) != a.defaults.end();
            if (inDefaults)
                continue;
            const auto focus = a.context == Context::global ? Context::playlist : a.context;
            CHECK (map.findUserBound (c, focus, false) == a.id);
            CHECK (map.find (c, focus, false) == a.id || map.findUserBound (c, focus, false) == a.id);
        }
}

// Round 165q's find: on Windows JUCE's F-keys / numpad are VK | 0x10000, and the editors' towupper() turned F4 into
// 'S' (the Slip tool) and F1 into 'P', so F4 (next empty pattern) did nothing with the Playlist focused.
TEST (issue12_function_keys_are_not_letters)
{
    using lynx::keys::letterOfKeyCode;
    for (int vk = 0x70; vk <= 0x7B; ++vk) // VK_F1…VK_F12, as JUCE's Windows key codes
        CHECK (letterOfKeyCode (vk | 0x10000) == 0);
    for (int vk = 0x60; vk <= 0x6F; ++vk) // the numpad
        CHECK (letterOfKeyCode (vk | 0x10000) == 0);
    for (int vk : { 0x25, 0x26, 0x27, 0x28, 0x21, 0x22, 0x24, 0x23, 0x2D, 0x2E }) // arrows, page, home, end, ins, del
        CHECK (letterOfKeyCode (vk | 0x10000) == 0);
    CHECK (letterOfKeyCode ('c') == 'C' && letterOfKeyCode ('S') == 'S' && letterOfKeyCode ('1') == '1');
    CHECK (letterOfKeyCode (' ') == 0 && letterOfKeyCode (0x7f) == 0);
}

// Bug: automation recorded in Song mode "cut itself": at the song's end the playhead went back to the start, the
// points wrapped onto the start of the take's clip, and a pause in the gesture began a new clip. Now (song mode,
// no loop region) the song plays on, the points keep their place, and the take's clip grows with the playhead.
TEST (issue12_song_mode_automation_take_grows_past_the_song_end)
{
    auto song = lynx::Song::makeDefault();
    song.mode = lynx::PlayMode::song;
    song.playlist.clear();
    song.automations.clear();
    lynx::Clip pc; // one bar of pattern: the song is one bar long
    pc.kind = lynx::ClipKind::pattern;
    pc.length = 4 * lynx::ppq;
    song.playlist.push_back (pc);
    const double bar = 4.0 * lynx::ppq;
    CHECK (lynx::songTickForRecorded (song, 1.5 * bar) == (std::int64_t) (0.5 * bar)); // (looping playback: wraps)
    CHECK (lynx::songTickForRecorded (song, 1.5 * bar, true) == (std::int64_t) (1.5 * bar));

    lynx::AutomationWrite w;
    w.target = { lynx::AutomationTargetKind::mixerVolume, 1 };
    w.name = "Insert 1 - Volume";
    lynx::commitAutomationPoints (song, w, { { 100, 0.2f }, { 400, 0.4f } }, 0, true);
    CHECK (w.createdByTake && w.automation == 0);
    auto clipOf = [&] () -> const lynx::Clip& {
        for (const auto& c : song.playlist)
            if (c.kind == lynx::ClipKind::automation)
                return c;
        return song.playlist.front();
    };
    CHECK (clipOf().start == 0 && clipOf().length == (std::int64_t) bar);
    // the playhead goes on (no move): the clip follows it, holding the value
    lynx::growTakeClip (song, w, (std::int64_t) (1.25 * bar), false);
    CHECK (clipOf().length == (std::int64_t) (1.25 * bar));
    // the gesture starts again in bar 3 (past the old song end): same clip, points where they were played
    w.lastLocal = -1; // (the engine: the first gesture ended)
    lynx::commitAutomationPoints (song, w, { { 2.5 * bar, 0.9f }, { 2.6 * bar, 1.0f } }, 0, true);
    int automationClips = 0;
    for (const auto& c : song.playlist)
        automationClips += c.kind == lynx::ClipKind::automation ? 1 : 0;
    CHECK (automationClips == 1 && song.automations.size() == 1);
    CHECK (clipOf().end() >= (std::int64_t) (2.6 * bar));
    const auto& a = song.automations[0];
    CHECK (std::abs (a.valueAt (150.0) - 0.25f) < 0.05f);                  // the start is not overwritten
    CHECK (std::abs (a.valueAt (1.2 * bar) - 0.4f) < 1e-6f);              // held between the gestures
    CHECK (std::abs (a.valueAt (2.6 * bar) - 1.0f) < 1e-6f);
    // recording stops at 2.7 bars: the clip ends at the end of that bar
    lynx::growTakeClip (song, w, (std::int64_t) (2.7 * bar), true);
    CHECK (clipOf().end() == 3 * (std::int64_t) bar);
}

// Bug: a controller's pitch wheel played but was not recorded. Recording writes it as the channel's pitch bend
// (MIDI pitch bend target, as for a hosted plugin); a LYNX instrument now plays that lane / clip as the wheel.
TEST (issue12_recorded_pitch_bend_plays_on_a_lynx_instrument)
{
    auto render = [] (bool withBend) {
        auto song = lynx::Song::makeDefault();
        song.mode = lynx::PlayMode::pattern;
        song.playlist.clear();
        auto& p = song.patterns[(std::size_t) song.currentPattern];
        p.notes.clear();
        lynx::Note n;
        n.channel = 4; // "Synth"
        n.key = 60;
        n.start = 0;
        n.length = 4 * lynx::ppq;
        p.notes.push_back (n);
        p.events.clear();
        if (withBend)
        {
            lynx::PatternEvents lane;
            lane.target = { lynx::AutomationTargetKind::channelParam, 4, 0, lynx::midictl::pitchBendBase };
            lane.events.push_back ({ 0, 1.0f }); // full bend up (+2 semitones, the channel's pitch range)
            p.events.push_back (lane);
        }
        auto state = lynx::RackState::build (song, nullptr, 48000.0);
        auto bound = false;
        for (const auto& b : state->pluginParams)
            bound = bound || (b.channel == 4 && b.param == lynx::midictl::pitchBendBase);
        lynx::RackRenderer renderer;
        renderer.prepare (48000.0, 512);
        std::vector<float> l (512), r (512), all;
        for (int block = 0; block < 16; ++block)
        {
            renderer.process (state.get(), true, block * 512.0 * lynx::ticksPerSample (song.bpm, 48000.0), 512, l.data(), r.data());
            all.insert (all.end(), l.begin(), l.end());
        }
        return std::make_pair (bound, all);
    };
    const auto plain = render (false), bent = render (true);
    CHECK (! plain.first && bent.first);
    // zero crossings over the same span: the bent note is higher (+2 semitones ≈ ×1.12)
    auto crossings = [] (const std::vector<float>& x) {
        int c = 0;
        for (std::size_t i = 4096; i + 1 < x.size(); ++i)
            c += (x[i] < 0.0f) != (x[i + 1] < 0.0f) ? 1 : 0;
        return c;
    };
    const auto a = crossings (plain.second), b = crossings (bent.second);
    CHECK (a > 50 && b > a);
}
