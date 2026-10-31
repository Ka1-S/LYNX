// Round 166e (issue #16): FL 2026's automatic crossfades, the audio clip gain handle, "Disarm on stop" and the Piano
// roll's Alt+wheel / note template (docs/analise/REA_rodada166e.md). Included at the end of tests.cpp.

namespace round166e
{
inline lynx::Clip audioClip (int track, std::int64_t start, std::int64_t length)
{
    lynx::Clip c;
    c.kind = lynx::ClipKind::audio;
    c.channel = 0;
    c.track = track;
    c.start = start;
    c.length = length;
    return c;
}

// one audio channel playing a constant 1.0 for 10 s; clips A (beats 0-4) and B (beats 2-6) on track 0
inline lynx::Song overlapSong (bool crossfades)
{
    lynx::Song s;
    s.bpm = 120.0; // 1 beat = 24000 samples at 48 kHz
    lynx::ChannelDef c;
    c.instrument = lynx::InstrumentKind::audioClip;
    c.samplePath = "ones";
    c.volume = 1.0f;
    s.channels.push_back (c);
    s.patterns.emplace_back();
    s.playlist.push_back (audioClip (0, 0, 4 * lynx::ppq));
    s.playlist.push_back (audioClip (0, 2 * lynx::ppq, 4 * lynx::ppq));
    s.mode = lynx::PlayMode::song;
    s.autoCrossfades = crossfades;
    lynx::clipops::updateCrossfades (s);
    return s;
}

// the left output at `beats` (rendered from the start in 256-sample blocks)
inline float levelAt (const lynx::Song& s, double beats)
{
    auto provider = [] (const std::string&) {
        auto d = std::make_shared<lynx::SampleData>();
        d->sampleRate = 48000.0;
        d->left.assign (48000 * 10, 1.0f);
        return std::shared_ptr<const lynx::SampleData> (d);
    };
    auto state = lynx::RackState::build (s, nullptr, 48000.0, provider);
    lynx::RackRenderer r;
    r.prepare (48000.0, 256);
    std::vector<float> l (256), rr (256);
    const auto tps = lynx::ticksPerSample (120.0, 48000.0);
    const auto blocks = (int) (beats * 24000.0 / 256.0);
    for (int b = 0; b <= blocks; ++b)
        r.process (state.get(), b == 0, (double) b * 256.0 * tps, 256, l.data(), rr.data());
    return l[255];
}
} // namespace round166e

TEST (round166e_automatic_crossfades_over_the_overlap)
{
    using namespace lynx;
    auto s = round166e::overlapSong (true);
    const auto& a = s.playlist[0];
    const auto& b = s.playlist[1];
    CHECK (a.crossfadeOut == 2 * ppq && a.crossfadeIn == 0);
    CHECK (b.crossfadeIn == 2 * ppq && b.crossfadeOut == 0);
    CHECK (a.effectiveFadeOut() == 2 * ppq && b.effectiveFadeIn() == 2 * ppq);

    // off: none (and a clip's own fades come back)
    s.autoCrossfades = false;
    s.playlist[0].fadeOut = 10;
    clipops::updateCrossfades (s);
    CHECK (s.playlist[0].crossfadeOut == 0 && s.playlist[1].crossfadeIn == 0 && s.playlist[0].effectiveFadeOut() == 10);

    // a clip lying inside another, clips on other tracks and touching clips get none
    std::vector<Clip> clips { round166e::audioClip (0, 0, 8 * ppq), round166e::audioClip (0, 2 * ppq, 2 * ppq), round166e::audioClip (1, 4 * ppq, 4 * ppq),
                              round166e::audioClip (2, 0, 4 * ppq), round166e::audioClip (2, 4 * ppq, 4 * ppq) };
    clipops::updateCrossfades (clips, true);
    for (const auto& c : clips)
        CHECK (c.crossfadeIn == 0 && c.crossfadeOut == 0);

    // a pattern clip doesn't take part
    std::vector<Clip> mixed { round166e::audioClip (0, 0, 4 * ppq), Clip { 0, 0, 2 * ppq, 4 * ppq, 0 } };
    clipops::updateCrossfades (mixed, true);
    CHECK (mixed[0].crossfadeOut == 0 && mixed[1].crossfadeIn == 0);

    // the order in the list doesn't matter (the later start fades in)
    std::vector<Clip> reversed { round166e::audioClip (0, 2 * ppq, 4 * ppq), round166e::audioClip (0, 0, 4 * ppq) };
    clipops::updateCrossfades (reversed, true);
    CHECK (reversed[0].crossfadeIn == 2 * ppq && reversed[1].crossfadeOut == 2 * ppq);
}

TEST (round166e_crossfade_curve_and_scheduler_shape)
{
    using namespace lynx;
    CHECK (clipops::crossfadeCurve (0.0f) == 0.0f && std::abs (clipops::crossfadeCurve (1.0f) - 1.0f) < 1e-6f);
    for (float x = 0.0f; x <= 1.0f; x += 0.125f) // the two sides add up to 1
        CHECK (std::abs (clipops::crossfadeCurve (x) + clipops::crossfadeCurve (1.0f - x) - 1.0f) < 1e-5f);
    CHECK (std::abs (clipops::crossfadeCurve (0.5f) - 0.5f) < 1e-6f);

    const auto s = round166e::overlapSong (true);
    const auto tps = ticksPerSample (120.0, 48000.0);
    EventBuffer ev;
    scheduleSong (s, 0.0, 6 * 24000, tps, ev);
    int ons = 0;
    for (const auto& e : ev)
        if (e.type == NoteEvent::Type::on)
        {
            ++ons;
            if (e.sampleOffset == 0)
                CHECK (e.fadeShape == 2 && std::abs (e.fadeOut - 2.0f * 24000.0f) < 1.0f);
            else
                CHECK (e.fadeShape == 1 && std::abs (e.fadeIn - 2.0f * 24000.0f) < 1.0f);
        }
    CHECK (ons == 2);
}

TEST (round166e_crossfade_keeps_the_level_through_the_overlap)
{
    const auto on = round166e::overlapSong (true);
    const auto off = round166e::overlapSong (false);
    const auto unity = round166e::levelAt (on, 1.0); // only clip A
    CHECK (unity > 0.1f);
    CHECK (std::abs (round166e::levelAt (off, 1.0) - unity) < 1e-4f);
    // without crossfades the overlap sums both clips; with them it stays at one clip's level (S curves adding to 1)
    CHECK (std::abs (round166e::levelAt (off, 3.0) - 2.0f * unity) < 0.01f * unity);
    CHECK (std::abs (round166e::levelAt (on, 3.0) - unity) < 0.01f * unity);
    CHECK (std::abs (round166e::levelAt (on, 2.5) - unity) < 0.01f * unity);
    CHECK (std::abs (round166e::levelAt (on, 4.5) - unity) < 0.01f * unity); // only clip B, past the overlap
}

TEST (round166e_gain_handle_range_and_gesture)
{
    using namespace lynx;
    using namespace lynx::gesture;
    CHECK (std::abs (clipops::clipGainToDb (1.0f)) < 1e-9);
    CHECK (clipops::clipGainToDb (0.0f) == clipops::clipGainMinDb);
    CHECK (clipops::clipGainToDb (1.0e-9f) == clipops::clipGainMinDb);
    CHECK (std::abs (clipops::clipGainToDb (clipops::clipGainFromDb (12.5)) - 12.5) < 1e-4);
    CHECK (std::abs (clipops::clipGainToDb (clipops::clipGainFromDb (99.0)) - 36.0) < 1e-4); // FL's top: +36 dB
    CHECK (std::abs (clipops::clipGainToDb (clipops::clipGainFromDb (-200.0)) - clipops::clipGainMinDb) < 1e-3);
    // FL: 0.05 dB per pixel, linear in dB (100 px = 5 dB)
    CHECK (std::abs (100 * clipops::clipGainDbPerPixel - 5.0) < 1e-12);

    const Mods none {};
    CHECK (leftAction (EditTool::pencil, Target::gainHandle, none, true) == Action::clipGain);
    CHECK (leftAction (EditTool::lasso, Target::gainHandle, none, true) == Action::clipGain);
    CHECK (cursorFor (Action::clipGain, EditTool::pencil) == Cursor::resizeV);
}

TEST (round166e_new_projects_get_automatic_crossfades)
{
    using namespace lynx;
    NewProjectDefaults d;
    CHECK (d.autoCrossfades); // FL's "Enable automatic crossfades in new projects" is on by default
    auto song = Song::makeDefault();
    applyNewProjectDefaults (song, d);
    CHECK (song.autoCrossfades);
    d.autoCrossfades = false;
    applyNewProjectDefaults (song, d);
    CHECK (! song.autoCrossfades);
    CHECK (d.toText().find ("newAutoCrossfades=0") != std::string::npos);
    NewProjectDefaults read;
    read.readKey ("newAutoCrossfades", "0");
    CHECK (! read.autoCrossfades);
    read.readKey ("newAutoCrossfades", "1");
    CHECK (read.autoCrossfades);
    CHECK (! Song {}.autoCrossfades); // a project saved without the flag loads with it off (FL)
}

TEST (round166e_disarm_on_stop_switches_recording_off)
{
    lynx::RecordOffOnStop sw;
    // on: recording while playing, then playback stops → the switch goes off once
    CHECK (! sw.update (false, true, true)); // armed, not playing yet
    CHECK (! sw.update (true, true, true));
    CHECK (! sw.update (true, true, true));
    CHECK (sw.update (false, true, true));
    CHECK (! sw.update (false, true, true));
    // off: nothing
    CHECK (! sw.update (true, true, false));
    CHECK (! sw.update (false, true, false));
    // playing without recording, then arming while stopped: nothing
    CHECK (! sw.update (true, false, true));
    CHECK (! sw.update (false, false, true));
    CHECK (! sw.update (false, true, true));
}

TEST (round166e_note_template_and_nearest_note)
{
    using namespace lynx;
    Note clicked;
    clicked.velocity = 0.3f;
    clicked.pan = -0.5f;
    clicked.release = 0.9f;
    clicked.finePitch = 20.0f;
    clicked.modX = 0.1f;
    clicked.modY = 0.8f;
    noteedit::NoteTemplate t;
    Note fresh;
    t.applyTo (fresh); // the defaults before any click
    CHECK (fresh.velocity == Note {}.velocity && fresh.pan == 0.0f);
    t.takeFrom (clicked);
    t.applyTo (fresh);
    CHECK (fresh.velocity == 0.3f && fresh.pan == -0.5f && fresh.release == 0.9f && fresh.finePitch == 20.0f && fresh.modX == 0.1f && fresh.modY == 0.8f);

    std::vector<Note> notes (3);
    notes[0].start = 0, notes[0].length = 96, notes[0].key = 60;
    notes[1].start = 960, notes[1].length = 96, notes[1].key = 72;
    notes[2].start = 480, notes[2].length = 96, notes[2].key = 40;
    const std::vector<int> all { 0, 1, 2 };
    const auto ppt = 24.0 / ticksPerStep;
    CHECK (noteedit::nearestNote (notes, all, 30.0, 60, ppt, 14.0) == 0);  // on it
    CHECK (noteedit::nearestNote (notes, all, 900.0, 70, ppt, 14.0) == 1); // near the second
    CHECK (noteedit::nearestNote (notes, all, 520.0, 45, ppt, 14.0) == 2);
    CHECK (noteedit::nearestNote (notes, { 0, 1 }, 520.0, 45, ppt, 14.0) >= 0);  // only candidates count
    CHECK (noteedit::nearestNote (notes, { 1 }, 0.0, 60, ppt, 14.0) == 1);
    CHECK (noteedit::nearestNote (notes, {}, 0.0, 60, ppt, 14.0) == -1);
    CHECK (noteedit::nearestNote (notes, all, 500.0, 127, ppt, 0.0) == 2); // the velocity lane: by time only
}
