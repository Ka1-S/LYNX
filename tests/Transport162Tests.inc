// Round 162 (issue #9) tests: partial quantize, the Piano Roll playhead in Song mode, Follow, Stop and the meter
// scale. Included at the end of tests.cpp.

#include "core/MeterBallistics.h"
#include "core/TransportEdit.h"

TEST (transport162_quantize_tick_strength)
{
    using lynx::transportedit::quantizeTick;
    // full strength: the nearest grid line (ties go up, as noteedit::quantize)
    CHECK (quantizeTick (130, 240, 1.0) == 240 && quantizeTick (110, 240, 1.0) == 0 && quantizeTick (120, 240, 1.0) == 240);
    // 50%: halfway there, both directions
    CHECK (quantizeTick (100, 240, 0.5) == 50);
    CHECK (quantizeTick (200, 240, 0.5) == 220);
    CHECK (quantizeTick (480, 240, 0.5) == 480); // on the grid: stays
    CHECK (quantizeTick (101, 240, 0.5) == 50 || quantizeTick (101, 240, 0.5) == 51);
    // applied again it keeps halving the distance and converges on the grid line
    auto t = std::int64_t (300);
    for (int i = 0; i < 12; ++i)
        t = quantizeTick (t, 240, 0.5);
    CHECK (t == 240);
    // 0 = nothing, never below 0, a zero grid is treated as one tick
    CHECK (quantizeTick (130, 240, 0.0) == 130 && quantizeTick (0, 240, 0.5) == 0 && quantizeTick (7, 0, 1.0) == 7);
    // strength is clamped
    CHECK (quantizeTick (100, 240, 3.0) == 0 && quantizeTick (100, 240, -1.0) == 100);
}

TEST (transport162_quantize_notes_and_clips)
{
    lynx::Pattern p;
    for (auto s : { 10, 250, 470 })
    {
        lynx::Note n;
        n.start = s;
        n.length = 77;
        p.notes.push_back (n);
    }
    lynx::transportedit::quantizeNoteStarts (p, { 0, 1, 2, 9 }, 240, 0.5);
    CHECK (p.notes[0].start == 5 && p.notes[1].start == 245 && p.notes[2].start == 475);
    CHECK (p.notes[0].length == 77 && p.notes[2].length == 77); // lengths kept

    // starts only, full strength: the same result as noteedit::quantize
    auto q = p;
    lynx::transportedit::quantizeNoteStarts (q, { 0, 1, 2 }, 240, 1.0);
    auto r = p;
    lynx::noteedit::quantize (r, { 0, 1, 2 }, 240);
    for (std::size_t i = 0; i < 3; ++i)
        CHECK (q.notes[i].start == r.notes[i].start);

    std::vector<lynx::Clip> clips (2);
    clips[0].start = 3840 + 500;
    clips[0].length = 1000;
    clips[0].offset = 96;
    clips[1].start = 100;
    lynx::transportedit::quantizeClipStarts (clips, { 0 }, 3840, 1.0);
    CHECK (clips[0].start == 3840 && clips[0].length == 1000 && clips[0].offset == 96 && clips[1].start == 100);
    lynx::transportedit::quantizeClipStarts (clips, { 1 }, 960, 0.5);
    CHECK (clips[1].start == 50);
}

TEST (transport162_piano_roll_playhead)
{
    using lynx::transportedit::patternPlayhead;
    lynx::Song song;
    song.beatsPerBar = 4;
    song.patterns.resize (2);
    const auto bar = (std::int64_t) lynx::ppq * 4;
    lynx::Note n;
    n.start = 0;
    n.length = bar; // pattern 0 is one bar long
    song.patterns[0].notes.push_back (n);
    song.currentPattern = 0;

    // Pattern mode: the current pattern loops; another pattern is not playing
    song.mode = lynx::PlayMode::pattern;
    const auto len = (double) song.patterns[0].effectiveLength (4);
    CHECK (std::abs (patternPlayhead (song, 0, len + 100.0) - 100.0) < 1e-9);
    CHECK (patternPlayhead (song, 1, 100.0) < 0.0);

    // Song mode: inside a clip of the pattern (offset counted), nothing outside
    song.mode = lynx::PlayMode::song;
    lynx::Clip c;
    c.pattern = 0;
    c.track = 2;
    c.start = 2 * bar;
    c.length = bar;
    c.offset = 240;
    song.playlist.push_back (c);
    lynx::Clip other = c;
    other.pattern = 1;
    other.start = 0;
    song.playlist.push_back (other);
    CHECK (std::abs (patternPlayhead (song, 0, (double) (2 * bar + 10)) - 250.0) < 1e-9);
    CHECK (patternPlayhead (song, 0, (double) (bar + 10)) < 0.0);
    CHECK (std::abs (patternPlayhead (song, 1, 10.0) - 250.0) < 1e-9);
    // two clips of the pattern at the same time: the upper track wins
    lynx::Clip upper = c;
    upper.track = 0;
    upper.offset = 0;
    song.playlist.push_back (upper);
    CHECK (std::abs (patternPlayhead (song, 0, (double) (2 * bar + 10)) - 10.0) < 1e-9);
    // the song loops: past its end the position wraps
    CHECK (std::abs (patternPlayhead (song, 0, (double) (song.songLength() + 2 * bar + 10)) - 10.0) < 1e-9);
    CHECK (patternPlayhead (song, 7, 10.0) < 0.0 && patternPlayhead (song, 0, -1.0) < 0.0);
}

TEST (transport162_follow_and_stop)
{
    using lynx::transportedit::followViewStart;
    // in view: unchanged; past the right edge (or its margin) or left of the view: a page jump
    CHECK (followViewStart (1000.0, 4000.0, 2000.0, 100.0) == 1000.0);
    CHECK (followViewStart (1000.0, 4000.0, 5200.0, 100.0) == 5100.0);
    CHECK (followViewStart (1000.0, 4000.0, 4950.0, 100.0, 100.0) == 4850.0);
    CHECK (followViewStart (1000.0, 4000.0, 500.0, 100.0) == 400.0);
    CHECK (followViewStart (1000.0, 4000.0, 50.0, 100.0) == 0.0);
    CHECK (followViewStart (1000.0, 4000.0, -1.0, 100.0) == 1000.0); // no playhead

    using lynx::transportedit::stopTarget;
    CHECK (stopTarget (false, false, 5000) == 0);
    CHECK (stopTarget (false, true, 5000) == 5000);
    CHECK (stopTarget (false, true, -1) == 0);
    CHECK (stopTarget (true, true, 5000) == 0); // the second Stop: the very start
}

TEST (transport162_meter_scale_marks)
{
    using namespace lynx::meters;
    CHECK (scalePosition (-60.0f) == 0.0f && scalePosition (6.0f) == 1.0f && std::abs (scalePosition (0.0f) - 60.0f / 66.0f) < 1e-6f);
    // a tall meter shows the whole FL-like set, top down
    const auto tall = scaleMarks (400.0f, 11.0f);
    CHECK (tall.front() == 3);
    for (auto db : { 3, 0, -3, -6, -12, -24, -36, -48 })
        CHECK (std::find (tall.begin(), tall.end(), db) != tall.end());
    CHECK (std::is_sorted (tall.begin(), tall.end(), [] (int a, int b) { return a > b; }));
    // a short one keeps 0 dB and never lets labels overlap
    for (auto h : { 40.0f, 80.0f, 140.0f, 220.0f })
    {
        const auto marks = scaleMarks (h, 11.0f);
        CHECK (std::find (marks.begin(), marks.end(), 0) != marks.end());
        for (std::size_t i = 1; i < marks.size(); ++i)
            CHECK ((scalePosition ((float) marks[i - 1]) - scalePosition ((float) marks[i])) * h >= 11.0f - 1e-3f);
    }
}
