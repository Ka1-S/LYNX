// Round 165f (rea165-markers): FL Studio's Playlist marker actions (docs/analise/REA_rodada165f-markers.md)

namespace markers165
{
inline lynx::Marker marker (std::int64_t tick, int action, const char* name = "")
{
    lynx::Marker m;
    m.tick = tick;
    m.name = name;
    m.action = action;
    return m;
}

inline lynx::Song eightBars()
{
    lynx::Song song;
    song.patterns.emplace_back().name = "P";
    lynx::Clip clip;
    clip.pattern = 0;
    clip.length = 8 * 4 * lynx::ppq;
    song.playlist.push_back (clip);
    song.mode = lynx::PlayMode::song;
    return song;
}

constexpr std::int64_t bar (int n) { return (std::int64_t) (n - 1) * 4 * lynx::ppq; }

// The audio thread's Song-mode transport (AudioEngine::audioDeviceIOCallbackWithContext) over `blocks` blocks of 512
// samples at 120 BPM / 48 kHz: the beats (0-based) whose start the position plays through, in order, and whether a
// pause stopped playback. `userJump` = { block, target tick, quantize } asks for a jump like a ruler click.
struct Played
{
    std::vector<int> beats;
    bool paused = false;
};
inline Played play (const lynx::Song& song, int blocks, double from = 0.0, std::tuple<int, double, int> userJump = { -1, 0.0, 0 })
{
    const auto plan = lynx::makeMarkerPlan (song);
    const double tps = 120.0 * lynx::ppq / (60.0 * 48000.0);
    const double end = (double) song.songLength();
    double position = from;
    if (position <= 0.0 && plan.start > 0) // AudioEngine::play: the Start marker
        position = (double) plan.start;
    double pendingAt = -1.0, pendingTo = -1.0;
    Played out;
    for (int b = 0; b < blocks && ! out.paused; ++b)
    {
        if (std::get<0> (userJump) == b)
        {
            pendingTo = std::get<1> (userJump);
            pendingAt = std::get<2> (userJump) > 0 ? (double) lynx::markers::quantizedJumpTick (song, position, std::get<2> (userJump)) : position;
        }
        for (int done = 0; done < 512 && ! out.paused;)
        {
            auto n = 512 - done;
            if (position >= end)
                position = (double) plan.wrapTo;
            n = std::clamp (n, 1, (int) std::ceil ((end - position) / tps));
            const auto step = lynx::markerStep (plan, position, n, tps, pendingAt);
            n = step.samples;
            // the beats this sub-block plays through
            const auto next = position + n * tps;
            for (auto beat = (std::int64_t) std::ceil (position / lynx::ppq); (double) beat * lynx::ppq < next; ++beat)
                out.beats.push_back ((int) beat);
            position = next;
            if (step.userJump && position >= pendingAt - 1e-6)
            {
                position = pendingTo;
                pendingAt = -1.0;
            }
            else if (step.jump != nullptr && position >= (double) step.jump->at - 1e-6)
            {
                if (step.jump->kind == lynx::MarkerJump::Kind::pause)
                {
                    position = (double) step.jump->at;
                    out.paused = true;
                }
                else
                    position = (double) step.jump->to;
            }
            else if (position >= end - 1e-6)
                position = (double) plan.wrapTo;
            done += n;
        }
    }
    return out;
}

inline std::vector<int> range (int from, int to) // [from, to)
{
    std::vector<int> v;
    for (int i = from; i < to; ++i)
        v.push_back (i);
    return v;
}
inline std::vector<int> operator+ (std::vector<int> a, const std::vector<int>& b)
{
    a.insert (a.end(), b.begin(), b.end());
    return a;
}
inline bool startsWith (const std::vector<int>& v, const std::vector<int>& prefix)
{
    return v.size() >= prefix.size() && std::equal (prefix.begin(), prefix.end(), v.begin());
}
} // namespace markers165

TEST (markers165_transport_follows_fl_sequences)
{
    using namespace markers165;
    // fl_skip_pause_play.png: Skip at bar 2, "Here" at bar 4, Pause at bar 6 -> bar 1, bars 4-5, stop at bar 6
    {
        auto song = eightBars();
        song.addMarker (marker (bar (2), lynx::Marker::markerSkip));
        song.addMarker (marker (bar (4), lynx::Marker::none));
        song.addMarker (marker (bar (6), lynx::Marker::markerPause));
        const auto p = play (song, 2000);
        CHECK (p.paused && p.beats == range (0, 4) + range (12, 20));
        // Play again from the pause: on to the song's end, then from the start
        const auto again = play (song, 400, (double) bar (6));
        CHECK (startsWith (again.beats, range (20, 32) + range (0, 4) + range (12, 20)));
    }
    // fl_marker_loop_section.png: Marker loop at bar 3, "End" at bar 5 -> 1..4, then 3, 4, 3, 4...
    {
        auto song = eightBars();
        song.addMarker (marker (bar (3), lynx::Marker::markerLoop));
        song.addMarker (marker (bar (5), lynx::Marker::none));
        const auto p = play (song, 1000); // 10.7 s = 21 beats
        CHECK (startsWith (p.beats, range (0, 16) + range (8, 16)));
        CHECK (! p.paused && p.beats.size() >= 21 && p.beats[20] == 12);
    }
    // fl_start_loop_play.png: Start at bar 2, Loop at bar 5 -> plays from bar 2 to the end, then from bar 5
    {
        auto song = eightBars();
        song.addMarker (marker (bar (2), lynx::Marker::start));
        song.addMarker (marker (bar (5), lynx::Marker::songLoop));
        const auto p = play (song, 1500);
        CHECK (startsWith (p.beats, range (4, 32) + range (16, 32) + range (16, 20)));
    }
    // fl_quantized_marker_click.png: Quantize song jumps = Bars, a jump to bar 4 asked mid bar 1 happens at bar 2
    {
        const auto song = eightBars();
        const auto p = play (song, 300, 0.0, { 50, (double) bar (4), 2 }); // block 50 = beat 1.07
        CHECK (startsWith (p.beats, range (0, 4) + range (12, 16)));
        const auto now = play (song, 300, 0.0, { 50, (double) bar (4), 0 }); // off: right away
        CHECK (startsWith (now.beats, range (0, 2) + range (12, 16)));
    }
    // FL's export: none of it (straight through)
    {
        auto song = eightBars();
        song.addMarker (marker (bar (3), lynx::Marker::markerLoop));
        std::int64_t music = 0, loop = 0;
        const auto render = lynx::exportopts::makeRenderSong (song, {}, 0, music, loop);
        CHECK (startsWith (play (render, 1600).beats, range (0, 32) + range (0, 4)));
    }
}

TEST (markers165_skip_jumps_to_the_next_marker_and_pause_stops_on_it)
{
    // [SHOT fl_skip_pause_play.png] FL: Skip at bar 2 -> bar 4 ("Here"), Pause at bar 6 stops there
    using namespace markers165;
    auto song = eightBars();
    song.addMarker (marker (bar (2), lynx::Marker::markerSkip, "Skip"));
    song.addMarker (marker (bar (4), lynx::Marker::none, "Here"));
    song.addMarker (marker (bar (6), lynx::Marker::markerPause, "Pause"));
    const auto plan = lynx::makeMarkerPlan (song);
    CHECK (plan.count == 2);
    CHECK (plan.jumps[0].at == bar (2) && plan.jumps[0].kind == lynx::MarkerJump::Kind::jump && plan.jumps[0].to == bar (4));
    CHECK (plan.jumps[1].at == bar (6) && plan.jumps[1].kind == lynx::MarkerJump::Kind::pause);

    // reached by playing through it, not when starting on it (Play after a pause goes on, as in FL)
    CHECK (plan.firstIn ((double) bar (2) - 10.0, (double) bar (2)) == &plan.jumps[0]);
    CHECK (plan.firstIn ((double) bar (2), (double) bar (2) + 100.0) == nullptr);
    CHECK (plan.firstIn ((double) bar (6), (double) bar (7)) == nullptr);
    CHECK (plan.firstIn ((double) bar (5), (double) bar (7)) == &plan.jumps[1]);

    // a skip with no marker after it goes to the song's end (where Song mode wraps)
    auto last = eightBars();
    last.addMarker (marker (bar (3), lynx::Marker::markerSkip));
    const auto lastPlan = lynx::makeMarkerPlan (last);
    CHECK (lastPlan.count == 1 && lastPlan.jumps[0].to == last.songLength());
}

TEST (markers165_plain_and_recording_markers_do_not_move_playback)
{
    using namespace markers165;
    auto song = eightBars();
    song.addMarker (marker (bar (2), lynx::Marker::none, "A"));
    song.addMarker (marker (bar (3), lynx::Marker::punchIn));
    song.addMarker (marker (bar (4), lynx::Marker::punchOut));
    song.addMarker ({ bar (5), {}, 3, 4 });
    const auto plan = lynx::makeMarkerPlan (song);
    CHECK (plan.count == 0 && plan.start < 0 && plan.wrapTo == 0);
}

TEST (markers165_start_and_loop_markers)
{
    using namespace markers165;
    auto song = eightBars();
    song.addMarker (marker (bar (3), lynx::Marker::start, "Start"));
    song.addMarker (marker (bar (5), lynx::Marker::songLoop, "Loop"));
    const auto plan = lynx::makeMarkerPlan (song);
    CHECK (plan.start == bar (3));
    CHECK (plan.wrapTo == bar (5));
    CHECK (plan.count == 0);
    // without a Loop marker the song starts over at the Start marker
    auto startOnly = eightBars();
    startOnly.addMarker (marker (bar (3), lynx::Marker::start));
    CHECK (lynx::makeMarkerPlan (startOnly).wrapTo == bar (3));
}

TEST (markers165_quantize_song_jumps)
{
    using namespace markers165;
    auto song = eightBars();
    // off: right away
    CHECK (lynx::markers::quantizedJumpTick (song, 100.0, 0) == 100);
    // beats: the next beat
    CHECK (lynx::markers::quantizedJumpTick (song, 100.0, 1) == 2 * lynx::ppq);
    CHECK (lynx::markers::quantizedJumpTick (song, (double) lynx::ppq, 1) == 2 * lynx::ppq);
    // bars: the next bar line; 2 / 4 / 8 bars: the next one of bars 1, 3, 5... / 1, 5, 9... / 1, 9...
    CHECK (lynx::markers::quantizedJumpTick (song, 100.0, 2) == bar (2));
    CHECK (lynx::markers::quantizedJumpTick (song, (double) bar (2) + 5.0, 3) == bar (3));
    CHECK (lynx::markers::quantizedJumpTick (song, (double) bar (2) + 5.0, 4) == bar (5));
    CHECK (lynx::markers::quantizedJumpTick (song, (double) bar (2) + 5.0, 5) == bar (9));
    // bars follow the time signature changes: 3/4 from bar 2
    song.addMarker ({ bar (2), {}, 3, 4 });
    CHECK (lynx::markers::quantizedJumpTick (song, (double) bar (2) + 5.0, 2) == bar (2) + 3 * lynx::ppq);
}

TEST (markers165_set_action)
{
    using namespace markers165;
    auto song = eightBars();
    auto m = marker (bar (2), lynx::Marker::none, "A");
    lynx::markers::setAction (song, m, lynx::Marker::timeSignature);
    CHECK (m.isSignature() && m.numerator == 4 && m.denominator == 4 && m.action == 0);
    lynx::markers::setAction (song, m, lynx::Marker::markerSkip);
    CHECK (! m.isSignature() && m.action == lynx::Marker::markerSkip);
    CHECK (std::string (lynx::markers::actionName (lynx::Marker::markerLoop)) == "Marker loop");
}

TEST (markers165_marker_loop_repeats_the_section_up_to_the_next_marker)
{
    // [SHOT fl_marker_loop_section.png] FL: Marker loop at bar 3, plain "End" at bar 5: 1..4, then 3, 4, 3, 4...
    // (no marker after it: the section runs to the song's end, fl_combo2_wrap.png)
    using namespace markers165;
    auto song = eightBars();
    song.addMarker (marker (bar (3), lynx::Marker::markerLoop, "ML"));
    song.addMarker (marker (bar (5), lynx::Marker::none, "End"));
    auto plan = lynx::makeMarkerPlan (song);
    CHECK (plan.count == 1 && plan.jumps[0].at == bar (5) && plan.jumps[0].to == bar (3));
    // reaching the marker itself does nothing
    CHECK (plan.firstIn ((double) bar (2), (double) bar (3) + 10.0) == nullptr);

    auto open = eightBars();
    open.addMarker (marker (bar (3), lynx::Marker::markerLoop));
    plan = lynx::makeMarkerPlan (open);
    CHECK (plan.count == 1 && plan.jumps[0].at == open.songLength() && plan.jumps[0].to == bar (3));

    // a loop and a skip on the same tick: the loop (end of the earlier section) wins
    auto both = eightBars();
    both.addMarker (marker (bar (3), lynx::Marker::markerLoop));
    both.addMarker (marker (bar (5), lynx::Marker::markerSkip));
    both.addMarker (marker (bar (7), lynx::Marker::none));
    plan = lynx::makeMarkerPlan (both);
    CHECK (plan.count == 2 && plan.firstIn ((double) bar (5) - 1.0, (double) bar (5)) == &plan.jumps[0] && plan.jumps[0].to == bar (3));
}

TEST (markers165_time_marker_menu_commands)
{
    using namespace markers165;
    auto song = eightBars();
    // "Add jump to next bar" at 6:05 (FL): a Marker skip there and a plain marker one bar later
    const auto at = bar (6) + lynx::ppq;
    lynx::markers::addJumpToNextBar (song, at);
    CHECK (song.markers().size() == 2 && song.markers()[0].tick == at && song.markers()[0].action == lynx::Marker::markerSkip);
    CHECK (song.markers()[1].tick == at + 4 * lynx::ppq && song.markers()[1].action == 0);
    auto plan = lynx::makeMarkerPlan (song);
    CHECK (plan.count == 1 && plan.jumps[0].at == at && plan.jumps[0].to == at + 4 * lynx::ppq);

    // "Add one every 2 bars" over the whole song: bars 1, 3, 5, 7 (named by their number)
    auto every = eightBars();
    lynx::markers::addEvery (every, 2);
    CHECK (every.markers().size() == 4 && every.markers()[1].tick == bar (3) && every.markers()[1].name == "3");
    lynx::markers::addEvery (every, 2); // no duplicates
    CHECK (every.markers().size() == 4);

    // "Change to marker loop": the time selection loops
    auto sel = eightBars();
    sel.loopStart = bar (2);
    sel.loopEnd = bar (4);
    lynx::markers::changeToMarkerLoop (sel);
    CHECK (! sel.hasLoop() && sel.markers().size() == 2 && sel.markers()[0].action == lynx::Marker::markerLoop);
    plan = lynx::makeMarkerPlan (sel);
    CHECK (plan.count == 1 && plan.jumps[0].at == bar (4) && plan.jumps[0].to == bar (2));
}

TEST (markers165_flp_import_keeps_the_actions)
{
    // FL's demo "Bombs Away" has a Loop marker (type 4); the importer keeps the action number and no invented name
    const auto path = std::filesystem::path (realFlpDir) / "Demo projects" / "Demo songs" / "Bombs Away ft KARRA - I'm Awake.flp";
    if (realFlpDir.empty() || ! std::filesystem::exists (path))
        return;
    std::ifstream in (path, std::ios::binary);
    const std::vector<std::uint8_t> file ((std::istreambuf_iterator<char> (in)), {});
    const auto song = lynx::importFlp (flp::parseProject (file));
    bool loop = false;
    for (const auto& a : song.arrangements)
        for (const auto& m : a.markers)
            loop = loop || m.action == lynx::Marker::songLoop;
    CHECK (loop);
    CHECK (lynx::makeMarkerPlan (song).wrapTo > 0);
}

TEST (markers165_export_plays_straight_through_like_fl)
{
    // [FL render] FL's renders of projects with every marker action equal the plain song: the export song keeps the
    // markers but drops their playback actions
    using namespace markers165;
    auto song = eightBars();
    song.addMarker (marker (bar (2), lynx::Marker::markerSkip));
    song.addMarker (marker (bar (4), lynx::Marker::markerLoop));
    song.addMarker (marker (bar (5), lynx::Marker::markerPause));
    song.addMarker (marker (bar (6), lynx::Marker::start));
    song.addMarker (marker (bar (7), lynx::Marker::songLoop));
    song.addMarker (marker (bar (7), lynx::Marker::punchIn));
    CHECK (lynx::makeMarkerPlan (song).count == 3);
    std::int64_t music = 0, loop = 0;
    const auto render = lynx::exportopts::makeRenderSong (song, {}, 0, music, loop);
    const auto plan = lynx::makeMarkerPlan (render);
    CHECK (plan.count == 0 && plan.start < 0 && plan.wrapTo == 0);
    CHECK (render.markers().size() == 6);
    CHECK (music == song.songLength());
}
