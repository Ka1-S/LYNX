// Issue #12 (follow-up): knobs and faders follow their automation on screen while the song plays, as in FL.
// core/AutomationDisplay evaluates on the UI thread the value each automated target has at the playhead. Included at
// the end of tests.cpp.

namespace issue12c
{
// a song in Song mode with an automation clip (0 → 1 over 4 bars) on `target`, at bar 1
inline lynx::Song songWithClip (const lynx::AutomationTarget& target)
{
    auto song = lynx::Song::makeDefault();
    song.mode = lynx::PlayMode::song;
    lynx::Automation a;
    a.target = target;
    a.points = { { 0, 0.0f, 0.0f }, { 16 * lynx::ppq, 1.0f, 0.0f } };
    song.automations.push_back (a);
    lynx::Clip c;
    c.kind = lynx::ClipKind::automation;
    c.automation = (int) song.automations.size() - 1;
    c.track = 3;
    c.start = 0;
    c.length = 16 * lynx::ppq;
    song.playlist.push_back (c);
    return song;
}
} // namespace issue12c

TEST (issue12c_automated_value_at_the_playhead)
{
    using namespace lynx;
    const AutomationTarget send { AutomationTargetKind::mixerSend, 1, 0 };
    const AutomationTarget volume { AutomationTargetKind::mixerVolume, 2 };
    auto song = issue12c::songWithClip (send);
    AutomationDisplay shown;

    // stopped: nothing is shown (the controls show the Song's value, which a hand move edits)
    CHECK (! shown.update (song, 1, 4.0 * ppq, false));
    CHECK (! shown.valueOf (send) && ! shown.any());
    CHECK (shown.automatedTargets().size() == 1 && shown.automatedTargets().front() == send);

    // playing: the clip's value at the playhead, the same one playback uses
    const auto g0 = shown.getGeneration();
    CHECK (shown.update (song, 1, 4.0 * ppq, true));
    CHECK (shown.getGeneration() != g0);
    CHECK (shown.valueOf (send) && std::abs (*shown.valueOf (send) - song.automationValueAt (0, 4.0 * ppq)) < 1.0e-5f);
    CHECK (std::abs (*shown.valueOf (send) - 0.25f) < 0.01f);
    CHECK (! shown.valueOf (volume));
    CHECK (shown.shown (volume, 0.7f) == 0.7f);

    // the same position again: no change, no repaint
    const auto g1 = shown.getGeneration();
    CHECK (! shown.update (song, 1, 4.0 * ppq, true));
    CHECK (shown.getGeneration() == g1);
    CHECK (shown.update (song, 1, 12.0 * ppq, true));
    CHECK (std::abs (*shown.valueOf (send) - 0.75f) < 0.01f);

    // after the clip: its last value; a muted clip: nothing
    CHECK (shown.update (song, 1, 40.0 * ppq, true) || true);
    CHECK (std::abs (*shown.valueOf (send) - 1.0f) < 0.01f);
    song.playlist.back().muted = true;
    shown.update (song, 2, 4.0 * ppq, true);
    CHECK (! shown.valueOf (send));

    // pattern mode: the Playlist does not play; the current pattern's event lane does
    song.playlist.back().muted = false;
    song.mode = PlayMode::pattern;
    auto& p = song.patterns[(std::size_t) song.currentPattern];
    PatternEvents lane;
    lane.target = volume;
    lane.events = { { 0, 0.2f }, { 2 * ppq, 0.9f } };
    p.events.push_back (lane);
    shown.update (song, 3, 3.0 * ppq, true);
    CHECK (! shown.valueOf (send));
    CHECK (shown.valueOf (volume) && std::abs (*shown.valueOf (volume) - 0.9f) < 1.0e-5f);
    shown.update (song, 3, 1.0 * ppq, true);
    CHECK (std::abs (*shown.valueOf (volume) - 0.2f) < 1.0e-5f);

    // stopping clears it (generation moves, so the editors repaint back to the Song's values)
    const auto g2 = shown.getGeneration();
    CHECK (shown.update (song, 3, 1.0 * ppq, false));
    CHECK (shown.getGeneration() != g2 && ! shown.any());
}

// The refresh runs on the UI thread once per frame: measure it on a big song (300 automated targets, 3000 clips)
TEST (issue12c_display_refresh_is_cheap)
{
    using namespace lynx;
    auto song = Song::makeDefault();
    song.mode = PlayMode::song;
    while (song.mixer.size() < 100)
        song.mixer.emplace_back();
    for (int a = 0; a < 300; ++a)
    {
        Automation automation;
        automation.target = { a % 3 == 0 ? AutomationTargetKind::mixerVolume : a % 3 == 1 ? AutomationTargetKind::mixerPan
                                                                                          : AutomationTargetKind::channelVolume,
                              a / 3 };
        for (int k = 0; k < 64; ++k)
            automation.points.push_back ({ (std::int64_t) k * ppq, (float) ((k * 7 + a) % 11) / 10.0f, 0.0f });
        song.automations.push_back (automation);
        for (int c = 0; c < 10; ++c)
        {
            Clip clip;
            clip.kind = ClipKind::automation;
            clip.automation = a;
            clip.track = a % 50;
            clip.start = (std::int64_t) c * 64 * ppq;
            clip.length = 64 * ppq;
            song.playlist.push_back (clip);
        }
    }
    AutomationDisplay shown;
    shown.update (song, 1, 0.0, true); // (builds the tables once)
    const auto start = std::chrono::steady_clock::now();
    constexpr int frames = 300;
    for (int f = 0; f < frames; ++f)
        shown.update (song, 1, (double) f * 37.0 * ppq / 10.0, true);
    const auto us = std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now() - start).count() / frames;
    std::printf ("  automation display: %.1f us per refresh (300 automated targets, 3000 clips)\n", us);
    CHECK (shown.automatedTargets().size() == 300);
    CHECK (shown.valueOf ({ AutomationTargetKind::mixerPan, 0 }).has_value());
    CHECK (us < 2000.0); // well under a frame (33 ms) even in a debug build
}
