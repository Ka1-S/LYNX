// Round 165f: FL's score logger (docs/analise/REA_rodada165f.md §3.1).

#include "core/ScoreLog.h"

TEST (score_log_ring_pairs_note_offs_and_drops_held_notes)
{
    using namespace lynx::scorelog;
    Log log (4);
    log.noteOn (0, 1, 60, 0.5f);
    log.noteOn (100, 1, 64, 0.6f);
    log.noteOff (200, 1, 60);
    log.noteOff (300, 2, 64); // other channel: ignored
    auto snap = log.snapshot();
    CHECK (snap.size() == 2 && snap[0].endMs == 200 && snap[1].endMs < 0);
    for (int i = 0; i < 5; ++i)
        log.noteOn (1000 + i, 0, 40 + i, 1.0f);
    snap = log.snapshot();
    CHECK (log.size() == 4 && snap.front().key == 41 && snap.back().key == 44); // the oldest are overwritten
    log.clear();
    CHECK (log.size() == 0 && log.snapshot().empty());
}

TEST (score_log_dump_follows_fl)
{
    using namespace lynx;
    using namespace lynx::scorelog;
    // at 120 BPM a beat is 500 ms = 960 ticks
    std::vector<Entry> log {
        { 10000, 10250, 0, 60, 0.8f },  // first note -> tick 0
        { 10500, 10500, 0, 62, 0.8f },  // zero length -> FL's minimum (a sixth of a step)
        { 11000, -1, 0, 64, 0.8f },     // still held: left out
        { 30000, 30500, 3, 67, 0.4f },  // 19.5 s after the previous note ended: the gap becomes two bars
    };
    DumpOptions o;
    o.minutes = 5;
    o.bpm = 120;
    o.beatsPerBar = 4;
    auto n = dump (log, o);
    CHECK (n.size() == 3);
    CHECK (n[0].start == 0 && n[0].length == 480 && n[0].key == 60);
    CHECK (n[1].start == 960 && n[1].length == ticksPerStep / 6);
    // the previous notes cover until tick 960 + 40; two bars (2 x 4 x 960) later
    CHECK (n[2].start == 1000 + 2 * 4 * ppq && n[2].channel == 3 && std::abs (n[2].velocity - 0.4f) < 1e-6f);
    // a short gap stays as played
    std::vector<Entry> close { { 0, 500, 0, 60, 1 }, { 1000, 1500, 0, 62, 1 } };
    n = dump (close, o);
    CHECK (n.size() == 2 && n[1].start == 1920);
    // the window reaches back from the end of the last note: one minute keeps only the last phrase
    std::vector<Entry> longAgo { { 0, 500, 0, 60, 1 }, { 200000, 200500, 0, 62, 1 } };
    o.minutes = 1;
    n = dump (longAgo, o);
    CHECK (n.size() == 1 && n[0].key == 62 && n[0].start == 0);
    CHECK (dump ({}, o).empty());
}

#include "core/Timecode.h"

TEST (smpte_timecode_labels)
{
    using namespace lynx::timecode;
    CHECK (format (0.0, 5) == "00:00:00:00");
    CHECK (format (3723.5, 2) == "01:02:03:12");  // 25 fps: half a second = frame 12
    CHECK (format (1.0, 1) == "00:00:01:00");
    // 29.97 drop frame (index 3): frame 1800 is labelled 00:01:00;02 (frames 0 and 1 of minute 1 are skipped)
    auto l = label (1800, rates[3]);
    CHECK (l.minutes == 1 && l.seconds == 0 && l.frames == 2);
    l = label (17982, rates[3]); // ten minutes: no skip at minute 10
    CHECK (l.minutes == 10 && l.seconds == 0 && l.frames == 0);
    l = label (1799, rates[3]);
    CHECK (l.minutes == 0 && l.seconds == 59 && l.frames == 29);
    // one real hour of 29.97 drop frame reads 01:00:00;00 (that is the point of drop frame)
    CHECK (format (3600.0, 3) == "01:00:00;00");
    // non-drop 29.97 runs slow: an hour reads 00:59:56:12
    CHECK (format (3600.0, 4) == "00:59:56:12");
    // 59.94 drop: 4 frames skipped
    l = label (3600, rates[8]);
    CHECK (l.minutes == 1 && l.frames == 4);
    CHECK (std::string (format (1.0, 3)).find (';') != std::string::npos);
}

#include "core/NoteNames.h"

TEST (note_names_english_germanic_solfege)
{
    using namespace lynx::notenames;
    CHECK (pitchName (10, Style::english, false) == "A#" && pitchName (10, Style::english, true) == "Bb");
    CHECK (pitchName (11, Style::germanic, false) == "H" && pitchName (10, Style::germanic, true) == "B");
    CHECK (pitchName (0, Style::solfege, false) == "Do" && pitchName (7, Style::solfege, false) == "Sol" && pitchName (3, Style::solfege, true) == "Mib");
    CHECK (pitchName (-1, Style::english, false) == "B");
    currentStyle.store ((int) Style::solfege);
    CHECK (keyName (60) == "Do5" && keyName (69) == "La5");
    currentStyle.store (0);
    CHECK (keyName (60) == "C5");
}
