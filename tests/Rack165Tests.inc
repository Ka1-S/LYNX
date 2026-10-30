// Round 165f sub-round rea165-rack: Channel Rack looping, Burn to pattern, Burn MIDI (docs/analise/REA_rodada165f-rack.md)

namespace rack165
{
using lynx::ChannelLoop;
using lynx::Note;
using lynx::Pattern;
namespace loops = lynx::channelloops;
constexpr std::int64_t step = lynx::ticksPerStep, beat = lynx::ppq, bar = 4 * lynx::ppq;

Note stepNote (int channel, int s)
{
    Note n;
    n.channel = channel;
    n.start = s * step;
    n.length = step;
    return n;
}

std::vector<std::int64_t> startsOf (const Pattern& p, int channel)
{
    std::vector<std::int64_t> out;
    for (const auto& n : p.notes)
        if (n.channel == channel)
            out.push_back (n.start);
    return out;
}
} // namespace rack165

TEST (rack165_loop_lengths_follow_fl_rules)
{
    using namespace rack165;
    Pattern p;
    p.lengthTicks = 2 * bar;
    p.notes = { stepNote (0, 0), stepNote (0, 4) };
    // FUN_012cfd20: the next step / beat / bar after the last tick the notes cover
    CHECK (loops::lastTick (p, 0) == 5 * step - 1);
    CHECK (loops::loopLength (p, 0, ChannelLoop::nextStep, 2 * bar, 4) == 5 * step);
    CHECK (loops::loopLength (p, 0, ChannelLoop::nextBeat, 2 * bar, 4) == 2 * beat);
    CHECK (loops::loopLength (p, 0, ChannelLoop::nextBar, 2 * bar, 4) == bar);
    CHECK (loops::loopLength (p, 0, 3, 2 * bar, 4) == 3 * step);           // N steps
    CHECK (loops::loopLength (p, 0, 99, 2 * bar, 4) == 2 * bar);           // at most the pattern
    CHECK (loops::loopLength (p, 0, ChannelLoop::dontLoop, 2 * bar, 4) == 0);
    CHECK (loops::loopLength (p, 0, ChannelLoop::nextBar, 2 * bar, 3) == 3 * beat); // 3/4 bars
    // "Set to next beat / bar": a fixed loop point in steps
    CHECK (loops::nextBoundarySteps (p, 0, ChannelLoop::nextBeat, 4) == 8);
    CHECK (loops::nextBoundarySteps (p, 0, ChannelLoop::nextBar, 4) == 16);
    // muted notes don't count
    p.notes.push_back (stepNote (0, 20));
    p.notes.back().muted = true;
    CHECK (loops::loopLength (p, 0, ChannelLoop::nextBar, 2 * bar, 4) == bar);
    CHECK (loops::settingText (ChannelLoop::dontLoop) == "..." && loops::settingText (-3) == "Bar" && loops::settingText (12) == "12");
}

TEST (rack165_loop_types_pick_the_channels)
{
    using namespace rack165;
    Pattern p;
    p.lengthTicks = 2 * bar;
    p.notes = { stepNote (0, 0), stepNote (0, 4) };
    Note held; // a Piano Roll note: two steps long
    held.channel = 1;
    held.length = 2 * step;
    p.notes.push_back (held);

    CHECK (loops::effectiveSetting (p, 0) == ChannelLoop::dontLoop); // looping off
    p.loopOn = true;
    p.loopMode = lynx::RackLoopMode::steps;
    CHECK (loops::effectiveSetting (p, 0) == ChannelLoop::nextBar);
    CHECK (loops::effectiveSetting (p, 1) == ChannelLoop::dontLoop); // not a step channel
    p.loopMode = lynx::RackLoopMode::all;
    CHECK (loops::effectiveSetting (p, 1) == ChannelLoop::nextBar);
    p.loopMode = lynx::RackLoopMode::advanced;
    CHECK (loops::effectiveSetting (p, 0) == ChannelLoop::dontLoop);
    loops::setSetting (p, 0, 3);
    loops::setSetting (p, 0, 6); // replaces
    CHECK (p.channelLoops.size() == 1 && loops::effectiveSetting (p, 0) == 6);
    p.loopMode = lynx::RackLoopMode::steps; // a stored point wins in every type
    CHECK (loops::effectiveSetting (p, 0) == 6);
}

TEST (rack165_expand_plays_polyrhythms)
{
    using namespace rack165;
    Pattern p;
    p.lengthTicks = bar;
    p.loopOn = true;
    p.loopMode = lynx::RackLoopMode::advanced;
    p.notes = { stepNote (0, 0), stepNote (0, 5), stepNote (1, 0) };
    loops::setSetting (p, 0, 3); // every 3 steps: the step at 5 is past the loop point and doesn't play
    loops::setSetting (p, 1, 5);
    auto played = p;
    loops::expandPattern (played, 4);
    CHECK ((startsOf (played, 0) == std::vector<std::int64_t> { 0, 3 * step, 6 * step, 9 * step, 12 * step, 15 * step }));
    CHECK ((startsOf (played, 1) == std::vector<std::int64_t> { 0, 5 * step, 10 * step, 15 * step }));
    CHECK (played.lengthTicks == bar && ! played.loopOn);
    // the pattern itself is untouched
    CHECK (startsOf (p, 0).size() == 2);

    // automatic length: a 1-bar drum loop under a 3-bar bass line repeats to the end; the length stays 3 bars
    Pattern q;
    q.loopOn = true;
    q.loopMode = lynx::RackLoopMode::all;
    q.notes = { stepNote (0, 0), stepNote (0, 8) };
    Note bass;
    bass.channel = 1;
    bass.start = 0;
    bass.length = 3 * bar - step;
    bass.key = 36;
    q.notes.push_back (bass);
    CHECK (q.effectiveLength (4) == 3 * bar);
    loops::expandPattern (q, 4);
    CHECK (startsOf (q, 0).size() == 6 && startsOf (q, 1).size() == 1);
    CHECK (q.effectiveLength (4) == 3 * bar);

    // a repeat that would run past the pattern's end is cut there
    Pattern r;
    r.lengthTicks = bar;
    r.loopOn = true;
    r.loopMode = lynx::RackLoopMode::advanced;
    Note longNote;
    longNote.length = 3 * step;
    r.notes = { longNote };
    loops::setSetting (r, 0, 6);
    loops::expandPattern (r, 4);
    CHECK (r.notes.size() == 3 && r.notes.back().start == 12 * step && r.notes.back().length == 3 * step);
    loops::setSetting (r, 0, 7);
}

TEST (rack165_burn_to_pattern)
{
    using namespace rack165;
    Pattern p;
    p.lengthTicks = bar;
    p.loopOn = true;
    p.loopMode = lynx::RackLoopMode::advanced;
    p.notes = { stepNote (0, 0), stepNote (1, 0), stepNote (2, 0) };
    loops::setSetting (p, 0, 4);
    loops::setSetting (p, 1, 8);
    CHECK ((loops::loopingChannels (p, 4) == std::vector<int> { 0, 1 }));
    CHECK (loops::burnChannel (p, 0, 4));
    CHECK (startsOf (p, 0).size() == 4 && loops::storedSetting (p, 0) == ChannelLoop::dontLoop);
    CHECK (! loops::burnChannel (p, 2, 4)); // not looping
    CHECK (loops::burnChannels (p, {}, 4) == 1); // the rest: channel 1
    CHECK (startsOf (p, 1).size() == 2 && loops::loopingChannels (p, 4).empty());
    // what plays is the same before and after
    auto before = p;
    loops::setSetting (before, 2, 2);
    auto played = before;
    loops::expandPattern (played, 4);
    loops::burnChannels (before, {}, 4);
    CHECK (startsOf (played, 2) == startsOf (before, 2));
}

TEST (rack165_loop_points_follow_channel_edits)
{
    using namespace rack165;
    lynx::Song song;
    song.channels.resize (3);
    song.patterns.resize (1);
    auto& p = song.patterns[0];
    p.notes = { stepNote (0, 0), stepNote (1, 0), stepNote (2, 0) };
    p.channelLoops = { { 0, 4 }, { 1, 5 }, { 2, 6 } };
    song.removeChannel (1);
    CHECK ((song.patterns[0].channelLoops == std::vector<ChannelLoop> { { 0, 4 }, { 1, 6 } }));
    lynx::permuteChannels (song, { 1, 0 });
    CHECK (loops::storedSetting (song.patterns[0], 0) == 6 && loops::storedSetting (song.patterns[0], 1) == 4);
    // the history names it
    auto after = song;
    after.patterns[0].loopOn = true;
    CHECK (std::string (lynx::describeChange (song, after).text) == "Channel looping: {0}");
}

TEST (rack165_flp_loop_events_import)
{
    using namespace rack165;
    FlpBuilder b;
    b.u8 (0x40); b.u16 (0); // channel 0
    b.u8 (0x40); b.u16 (1); // channel 1
    b.u8 (0x41); b.u16 (1); // pattern 1
    auto notes = noteRecord (0, 0, 0, 60, 100);
    const auto second = noteRecord (24 * 4, 1, 0, 60, 100);
    notes.insert (notes.end(), second.begin(), second.end());
    b.data (0xE0, notes);
    b.u8 (0xA0); b.u32 (0); b.u8 (0xA1); b.u32 ((std::uint32_t) -3); // channel 0: Bar
    b.u8 (0xA0); b.u32 (1); b.u8 (0xA1); b.u32 (12);                // channel 1: 12 steps
    b.u8 (0x41); b.u16 (1);
    b.u8 (0x1A); b.u8 (1);       // loop on
    b.u8 (0xA2); b.u32 (3);      // advanced
    const auto project = flp::parseProject (b.bytes);
    CHECK (project.patterns.size() == 1 && project.patterns[0].loopOn && project.patterns[0].loopMode == 3);
    CHECK ((project.patterns[0].channelLoops == std::vector<std::pair<int, int>> { { 0, -3 }, { 1, 12 } }));
    const auto song = lynx::importFlp (project);
    const auto* pat = &song.patterns.back();
    for (const auto& q : song.patterns)
        if (! q.notes.empty())
            pat = &q;
    CHECK (pat->loopOn && pat->loopMode == lynx::RackLoopMode::advanced);
    CHECK (loops::effectiveSetting (*pat, 0) == ChannelLoop::nextBar && loops::effectiveSetting (*pat, 1) == 12);
}

TEST (rack165_fl_demo_song_loops)
{
    // FL's own demo "Song For You" saves its pattern 10 with looping on (type 1) and every channel at "Bar" [DATA]
    if (realFlpDir.empty())
        return;
    const auto path = realFlpDir + "/Demo projects/Demo songs/Edlan & Ella Noël - Song For You.flp";
    std::ifstream in (path, std::ios::binary);
    if (! in)
        return;
    std::vector<std::uint8_t> bytes ((std::istreambuf_iterator<char> (in)), {});
    const auto project = flp::parseProject (bytes);
    const flp::Pattern* looped = nullptr;
    for (const auto& p : project.patterns)
        if (p.loopOn)
            looped = &p;
    CHECK (looped != nullptr && looped->index == 10 && looped->loopMode == 1 && looped->channelLoops.size() >= 10);
    const auto song = lynx::importFlp (project);
    int on = 0;
    for (const auto& p : song.patterns)
        on += p.loopOn;
    CHECK (on == 1);
}

TEST (rack165_burn_midi_bakes_echo_swing_and_time)
{
    using namespace rack165;
    lynx::Song song;
    song.channels.resize (1);
    song.patterns.resize (1);
    song.patterns[0].lengthTicks = 2 * bar;
    auto n = stepNote (0, 0);
    n.velocity = 0.8f;
    song.patterns[0].notes = { n };
    auto& c = song.channels[0];
    c.misc.echo.feedback = 0.5f;
    c.misc.echo.echoes = 3;
    c.misc.echo.timeBeats = 0.75f;
    CHECK (lynx::channelburn::whatBurns (song, 0, 0).echo);
    auto played = lynx::channelburn::playedNotes (song, 0, 0);
    CHECK (played.size() == 4);
    if (played.size() == 4)
    {
        CHECK (played[0].start == 0 && played[1].start == 720 && played[2].start == 1440 && played[3].start == 2160);
        CHECK (played[0].velocity == 0.8f && played[1].velocity < played[0].velocity && played[3].velocity < played[2].velocity);
        CHECK (played[1].length == step);
    }
    // swing and Misc › Time: the step at 1 moves half a step at full swing, then the shift; the gate cuts it
    lynx::Song s2;
    s2.channels.resize (1);
    s2.patterns.resize (1);
    s2.patterns[0].lengthTicks = bar;
    s2.patterns[0].notes = { stepNote (0, 1) };
    s2.patterns[0].notes[0].length = 2 * step;
    s2.swing = 1.0f;
    s2.channels[0].misc.shiftBeats = 0.25f;
    s2.channels[0].misc.gateBeats = 0.125f;
    played = lynx::channelburn::playedNotes (s2, 0, 0);
    CHECK (played.size() == 1 && played[0].start == step + step / 2 + step && played[0].length == step / 2);
    CHECK (lynx::channelburn::burnMidi (s2, 0, false, {}));
    CHECK (s2.patterns[0].notes.size() == 1 && s2.channels[0].swingMix == 0.0f && s2.channels[0].misc.shiftBeats == 0.0f);
    // nothing to burn
    s2.patterns[0].notes.clear();
    CHECK (! lynx::channelburn::burnMidi (s2, 0, false, {}));
}

TEST (rack165_burn_midi_arpeggio_and_new_pattern)
{
    using namespace rack165;
    lynx::Song song;
    song.channels.resize (2);
    song.patterns.resize (1);
    song.patterns[0].lengthTicks = bar;
    for (int key : { 60, 64, 67 })
    {
        Note n;
        n.channel = 0;
        n.key = key;
        n.length = bar;
        song.patterns[0].notes.push_back (n);
    }
    song.patterns[0].notes.push_back (stepNote (1, 0));
    auto& c = song.channels[0];
    c.arp.direction = 1; // up
    c.arp.range = 1;
    c.arp.stepBeats = 0.25f;
    c.arp.gate = 0.5f;
    const auto played = lynx::channelburn::playedNotes (song, 0, 0);
    CHECK (played.size() == 16);
    bool grid = true, keys = true;
    for (std::size_t i = 0; i < played.size(); ++i)
    {
        grid = grid && played[i].start == (std::int64_t) i * step && played[i].length == step / 2;
        keys = keys && played[i].key == std::array<int, 3> { 60, 64, 67 }[i % 3];
    }
    CHECK (grid);
    CHECK (keys);
    // into a new pattern: the original keeps its chord, the arpeggiator is reset
    CHECK (lynx::channelburn::burnMidi (song, 0, true, {}));
    CHECK (song.patterns.size() == 2 && song.currentPattern == 1 && song.patterns[1].notes.size() == 16);
    CHECK (startsOf (song.patterns[0], 0).size() == 3 && ! song.channels[0].arp.enabled());
    CHECK (song.patterns[1].lengthTicks == bar);
}

// "Create DirectWave instrument..." (TDWPRenderForm): the plan follows FL's defaults
TEST (rack165_autosampler_plan)
{
    namespace as = lynx::autosample;
    as::Options o; // 36..83, 4 keys per zone, root offset 2, 1 layer
    auto p = as::plan (o, 120.0);
    CHECK (p.takes.size() == 12);
    CHECK (p.takes[0].key == 38 && p.takes[0].lowKey == 36 && p.takes[0].highKey == 39 && p.takes[0].velocity == 127);
    CHECK (p.takes[11].lowKey == 80 && p.takes[11].highKey == 83 && p.takes[11].lowVelocity == 0 && p.takes[11].highVelocity == 127);
    CHECK (p.holdTicks == 2 * 2 * lynx::ppq);                 // 2 s at 120 BPM = 4 beats
    CHECK (p.slotTicks == 6 * 2 * lynx::ppq);                 // hold + max = 6 s
    CHECK (p.takes[1].startTick == p.slotTicks);
    CHECK (std::abs (as::renderSeconds (p) - 72.0) < 1e-9);

    o.velocityLayers = 2;
    o.lowKey = 60;
    o.highKey = 62;
    o.keysPerZone = 2;
    o.rootOffset = 5; // clamped into the zone
    p = as::plan (o, 120.0);
    CHECK (p.takes.size() == 4);
    CHECK (p.takes[0].velocity == 64 && p.takes[0].lowVelocity == 0 && p.takes[0].highVelocity == 64);
    CHECK (p.takes[1].velocity == 127 && p.takes[1].lowVelocity == 65 && p.takes[1].highVelocity == 127);
    CHECK (p.takes[0].key == 61 && p.takes[2].lowKey == 62 && p.takes[2].highKey == 62 && p.takes[2].key == 62);
    CHECK (as::takeFileName ("Pad: 1", p.takes[0]) == "Pad_ 1 C#5 v64.wav");

    const auto zones = as::zones (p, { "a", "b", "c", "d" });
    CHECK (zones.size() == 4 && zones[1].path == "b" && zones[1].rootKey == 61 && zones[1].lowVelocity == 65);
}

TEST (rack165_autosampler_render_song_and_cuts)
{
    namespace as = lynx::autosample;
    lynx::Song song;
    song.bpm = 120.0;
    song.channels.resize (3);
    song.channels[1].volume = 0.5f;
    song.channels[1].pan = 0.4f;
    song.channels[1].mixerTrack = 3;
    song.channels[1].name = "Lead";
    song.patterns.resize (1);
    as::Options o;
    o.lowKey = 60;
    o.highKey = 67;
    o.holdSeconds = 0.5;
    o.maxSeconds = 0.5;
    const auto p = as::plan (o, song.bpm);
    CHECK (p.takes.size() == 2);
    const auto r = as::makeRenderSong (song, 1, p, o);
    CHECK (r.channels[0].muted && r.channels[2].muted && ! r.channels[1].muted);
    CHECK (r.channels[1].volume == 1.0f && r.channels[1].pan == 0.0f && r.channels[1].mixerTrack == 0);
    CHECK (r.currentPattern == 1 && r.mode == lynx::PlayMode::pattern && r.patterns[1].notes.size() == 2);
    CHECK (r.patterns[1].notes[1].start == p.slotTicks && r.patterns[1].notes[1].key == 66 && r.patterns[1].lengthTicks == 2 * p.slotTicks);

    // a render: take 1 rings 0.2 s, take 2 0.6 s (at 1 kHz)
    const double rate = 1000.0;
    const int slot = (int) std::lround ((double) p.slotTicks / lynx::ppq * 0.5 * rate);
    std::vector<float> left ((std::size_t) (2 * slot), 0.0f), right = left;
    for (int i = 0; i < 200; ++i)
        left[(std::size_t) i] = 0.25f;
    for (int i = 0; i < 600; ++i)
        right[(std::size_t) (slot + i)] = -0.5f;
    auto cuts = as::cutTakes (left.data(), right.data(), (int) left.size(), rate, p, o);
    CHECK (cuts.size() == 2);
    CHECK (cuts[0].left.size() == 250 && cuts[0].right.size() == 250); // + 50 ms of tail
    CHECK (cuts[1].left.size() == 650);
    CHECK (cuts[0].left.back() == 0.0f);                                 // declicked out

    o.normalize = true;
    o.channels = 1; // mono merged
    o.declickOut = false;
    cuts = as::cutTakes (left.data(), right.data(), (int) left.size(), rate, p, o);
    CHECK (cuts[0].right.empty() && std::abs (cuts[1].left[10] + 1.0f) < 1e-6f && std::abs (cuts[0].left[10] - 0.5f) < 1e-6f);

    o.stopOnSilence = false;
    cuts = as::cutTakes (left.data(), right.data(), (int) left.size(), rate, p, o);
    CHECK ((int) cuts[0].left.size() == slot);

    const auto c = as::makeSamplerChannel (song.channels[1], as::zones (p, { "x.wav", "y.wav" }), o);
    CHECK (c.name == "Lead (sampled)" && c.instrument == lynx::InstrumentKind::sampler && c.zones.size() == 2 && c.mixerTrack == 3);
    CHECK (c.volume == 0.5f && c.pan == 0.4f && c.samplePath == "x.wav");
}
