// --- REA round 146b: FL's Misc tab (time shift / gate, echo delay, tracking, arpeggiator) and the MIDI import window ---

TEST (rea146b_fl_time_knob_scale)
{
    using namespace lynx;
    // one step at 1024; FL's renders of Misc › Shift at 96 PPQ: 1000 → 22 ticks, 800 → 11, 512 → 4, 400 → 2, 200 → 1, 96 → 0
    CHECK (std::abs (channelmisc::flTimeKnobBeats (1024) - 0.25) < 1e-9);
    const std::pair<int, int> shifts[] = { { 1000, 22 }, { 800, 11 }, { 512, 4 }, { 400, 2 }, { 200, 1 }, { 96, 0 } };
    for (const auto& [v, ticks] : shifts)
        CHECK (std::nearbyint (channelmisc::flTimeKnobBeats (v) * 96.0) == ticks);
    // the arpeggiator's time 640 → 32.8 ms at 120 BPM [EXEC]
    CHECK (std::abs (channelmisc::flTimeKnobBeats (640) * 500.0 - 32.8) < 0.3);
    CHECK (channelmisc::flTimeKnobValue (channelmisc::flTimeKnobBeats (777)) == 777);
}

TEST (rea146b_shift_and_gate_schedule)
{
    using namespace lynx;
    Song song = Song::makeDefault();
    song.patterns[0].notes.clear();
    song.patterns[0].notes.push_back ({ 0, ppq, 0, 60, 0.8f });
    song.channels[0].misc.shiftBeats = 0.25f; // one step later
    song.channels[0].misc.gateBeats = 0.125f; // at most half a step
    EventBuffer ev;
    const auto tps = ticksPerSample (120.0, 48000.0);
    schedulePatternLoop (song.patterns[0], song.patterns[0].effectiveLength (4), 0.0, 48000, tps, ev, 0.0f, &song.channels);
    double on = -1, off = -1;
    for (const auto& e : ev)
        (e.type == NoteEvent::Type::on ? on : off) = e.sampleOffset * tps;
    CHECK (std::abs (on - ticksPerStep) < 2.0);
    CHECK (std::abs (off - (ticksPerStep + ppq / 8)) < 2.0);
    // a porta note with Full porta keeps its length
    song.patterns[0].notes[0].porta = true;
    ev.clear();
    schedulePatternLoop (song.patterns[0], song.patterns[0].effectiveLength (4), 0.0, 48000, tps, ev, 0.0f, &song.channels);
    for (const auto& e : ev)
        if (e.type == NoteEvent::Type::off)
            CHECK (std::abs (e.sampleOffset * tps - (ticksPerStep + ppq)) < 2.0);
}

TEST (rea146b_echo_delay_events)
{
    using namespace lynx;
    EchoSettings s;
    s.feedback = 0.5f;
    s.echoes = 4;
    s.timeBeats = 0.5f;
    s.pan = -0.5f;
    s.pitch = 100.0f;
    auto run = [&] (const EchoSettings& settings) {
        channelmisc::NoteEcho echo;
        std::vector<std::tuple<double, NoteEvent, float>> out;
        const auto tps = ticksPerSample (120.0, 48000.0);
        for (int block = 0; block < 100; ++block)
        {
            NoteEvent in[2];
            int count = 0;
            if (block == 0)
            {
                in[0].type = NoteEvent::Type::on;
                in[0].key = 60;
                in[0].velocity = 0.8f;
                in[1] = in[0];
                in[1].type = NoteEvent::Type::off;
                in[1].sampleOffset = 100;
                count = 2;
            }
            int n = 0;
            echo.process (0, in, count, block * 512 * tps, tps, 512, settings, echo.output.data(), echo.outputGains.data(), n);
            for (int i = 0; i < n; ++i)
                out.emplace_back ((block * 512 + echo.output[(std::size_t) i].sampleOffset) * tps, echo.output[(std::size_t) i], echo.outputGains[(std::size_t) i]);
        }
        return out;
    };
    const auto out = run (s);
    std::vector<std::tuple<double, NoteEvent, float>> ons;
    for (const auto& o : out)
        if (std::get<1> (o).type == NoteEvent::Type::on)
            ons.push_back (o);
    CHECK (ons.size() == 5 && out.size() == 10);
    const float pans[] = { 0.0f, -0.5f, -1.0f, -1.0f, -1.0f };
    for (std::size_t k = 0; k < ons.size() && k < 5; ++k)
    {
        CHECK (std::abs (std::get<0> (ons[k]) - k * ppq * 0.5) < 2.0);           // 0.5 beat apart
        CHECK (std::abs (std::get<2> (ons[k]) - std::pow (0.5f, (float) k)) < 1e-6f); // feedback^k
        CHECK (std::abs (std::get<1> (ons[k]).pan - pans[k]) < 1e-6f);             // k pan steps, clamped
        CHECK (std::get<1> (ons[k]).key == 60 + (int) k);                           // k · 100 cents
    }
    // ping pong bounces the pan: −0.5, −1, −0.5, 0 [EXEC]
    s.pingPong = true;
    s.pitch = 0.0f;
    const float bounced[] = { 0.0f, -0.5f, -1.0f, -0.5f, 0.0f };
    int k = 0;
    for (const auto& o : run (s))
        if (std::get<1> (o).type == NoteEvent::Type::on && k < 5)
            CHECK (std::abs (std::get<1> (o).pan - bounced[k++]) < 1e-6f);
    CHECK (k == 5);
}

TEST (rea146b_tracking_pan)
{
    using namespace lynx;
    ChannelMisc m;
    m.key.pan = 1.0f; // FL 64
    // FL [EXEC]: ±12 semitones from C5 → ±0.25
    CHECK (std::abs (channelmisc::trackedPan (m, 72, 0.78f, 0.0f) - 0.25f) < 1e-6f);
    CHECK (std::abs (channelmisc::trackedPan (m, 48, 0.78f, 0.0f) + 0.25f) < 1e-6f);
    m.key.mid = 72; // FL plays the "Mid" mirrored: no offset at 48
    CHECK (std::abs (channelmisc::trackedPan (m, 48, 0.78f, 0.0f)) < 1e-6f);
    ChannelMisc v;
    v.velocity.pan = 0.5f; // FL 32, mid 100: velocity 128 → +0.145, 50 → −0.106 [EXEC]
    CHECK (std::abs (channelmisc::trackedPan (v, 60, 1.0f, 0.0f) - 0.145f) < 0.005f);
    CHECK (std::abs (channelmisc::trackedPan (v, 60, 50.0f / 128.0f, 0.0f) + 0.106f) < 0.005f);
    CHECK (std::abs (channelmisc::trackedPan (v, 60, 100.0f / 128.0f, 0.3f) - 0.3f) < 1e-6f);
}

namespace
{
std::vector<int> arpKeys (const lynx::ArpSettings& a, std::vector<int> chord, int startTick, double beats, std::vector<double>* times = nullptr)
{
    using namespace lynx;
    Arpeggiator arp;
    const auto tps = ticksPerSample (120.0, 48000.0);
    std::vector<int> keys;
    for (double tick = 0.0; tick < beats * ppq; tick += 512 * tps)
    {
        int in = 0;
        for (auto key : chord)
            if (startTick >= tick && startTick < tick + 512 * tps)
            {
                NoteEvent e;
                e.type = NoteEvent::Type::on;
                e.key = key;
                e.velocity = 0.8f;
                e.sampleOffset = (int) ((startTick - tick) / tps);
                arp.input[(std::size_t) in++] = e;
            }
        int out = 0;
        arp.process (0, arp.input.data(), in, tick, tps, 512, a, arp.output.data(), out);
        for (int i = 0; i < out; ++i)
            if (arp.output[(std::size_t) i].type == NoteEvent::Type::on || arp.output[(std::size_t) i].type == NoteEvent::Type::slide)
            {
                keys.push_back (arp.output[(std::size_t) i].key);
                if (times != nullptr)
                    times->push_back (tick + arp.output[(std::size_t) i].sampleOffset * tps);
            }
    }
    return keys;
}
} // namespace

TEST (rea146b_arpeggiator_like_fl)
{
    using namespace lynx;
    ArpSettings a;
    a.direction = 5; // FL's up-down "sticky": the ends twice [EXEC]
    a.stepBeats = 0.25f;
    a.gate = 1.0f;
    auto keys = arpKeys (a, { 60, 64, 67 }, 0, 2.0);
    const int sticky[] = { 60, 64, 67, 67, 64, 60, 60, 64 };
    CHECK (keys.size() >= 8);
    for (std::size_t i = 0; i < 8 && i < keys.size(); ++i)
        CHECK (keys[i] == sticky[i]);
    // the steps count from the chord's start (FL [EXEC]: a chord at tick 10 of 96 arpeggiates from there)
    a.direction = 1;
    std::vector<double> times;
    arpKeys (a, { 60, 64, 67 }, ppq / 10, 1.0, &times);
    CHECK (times.size() >= 3 && std::abs (times[0] - ppq / 10) < 2.0 && std::abs (times[1] - (ppq / 10 + ppq / 4)) < 2.0);
    // gate past 100 %: the next note waits (step = time · gate)
    a.gate = 64.0f / 48.0f;
    times.clear();
    arpKeys (a, { 60, 64, 67 }, 0, 2.0, &times);
    CHECK (times.size() >= 2 && std::abs (times[1] - ppq / 4 * 64.0 / 48.0) < 2.0);
    // FL's Repeat: the sequence twice per octave before the next (range 2)
    a.gate = 1.0f;
    a.range = 2;
    a.octaveRepeat = 2;
    keys = arpKeys (a, { 60, 64, 67 }, 0, 3.0);
    const int repeated[] = { 60, 64, 67, 60, 64, 67, 72, 76, 79, 72 };
    CHECK (keys.size() >= 10);
    for (std::size_t i = 0; i < 10 && i < keys.size(); ++i)
        CHECK (keys[i] == repeated[i]);
    // a chord from FL's Chords.map on one note (index 5 = minor [EXEC]); the slide glides without new attacks
    const std::string map = "Chords,-Chords\r\nMajor,Major,4,3\r\nSuspended 2nd,sus2,2,5\r\nSuspended 4th,sus4,5,2\r\nMajor Flat 5th,Majb5,4,2\r\nminor,minor,3,4\r\n";
    CHECK ((channelmisc::chordFromFlMap (map, 5) == std::vector<int> { 0, 3, 7 }));
    CHECK ((channelmisc::chordFromFlMap (map, 1) == std::vector<int> { 0, 4, 7 }));
    CHECK (channelmisc::chordFromFlMap (map, 9).empty());
    a.range = 1;
    a.octaveRepeat = 1;
    a.chord = channelmisc::chordFromFlMap (map, 5);
    keys = arpKeys (a, { 60 }, 0, 1.0);
    CHECK (keys.size() >= 4 && keys[0] == 60 && keys[1] == 63 && keys[2] == 67 && keys[3] == 60);
}

TEST (rea146b_flp_misc_records)
{
    using namespace lynx;
    if (realFlpDir.empty())
        return;
    std::ifstream in (std::filesystem::path (realFlpDir) / "System" / "Render tests" / "Sampler.flp", std::ios::binary);
    std::vector<std::uint8_t> b ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());
    if (b.empty())
        return;
    // patch records in place (same sizes): 0x59 shift 1000, 0xD1 echo, 0xD7 arpeggiator / flags / gate / range,
    // 0xE4 key tracking pan 32
    auto put32 = [&] (std::size_t at, std::int32_t v) { std::memcpy (b.data() + at, &v, 4); };
    std::size_t p = 22; // after FLhd (14) + FLdt + size
    int tracking = 0;
    while (p < b.size())
    {
        const auto id = b[p++];
        if (id < 0x40) { p += 1; continue; }
        if (id < 0x80) { if (id == 0x59) { b[p] = 1000 & 0xFF; b[p + 1] = 1000 >> 8; } p += 2; continue; }
        if (id < 0xC0) { p += 4; continue; }
        std::size_t n = 0;
        for (int shift = 0;; shift += 7) { const auto x = b[p++]; n |= (std::size_t) (x & 0x7F) << shift; if (! (x & 0x80)) break; }
        if (id == 0xD1)
        {
            put32 (p, 6400); put32 (p + 4, 3200); put32 (p + 8, 100); put32 (p + 12, 3); put32 (p + 16, 96);
        }
        else if (id == 0xD7 && n >= 116)
        {
            b[p + 10] = 2; // ping pong
            b[p + 11] = 0; // main pitch off
            put32 (p + 40, 4); put32 (p + 44, 2); put32 (p + 48, 5); put32 (p + 52, 896); put32 (p + 56, 24);
            b[p + 60] = 1; // slide
            b[p + 62] = 0; // full porta off
            b[p + 64] = 600 & 0xFF; b[p + 65] = 600 >> 8;
            put32 (p + 92, 2);
            put32 (p + 112, 5);
        }
        else if (id == 0xE4 && tracking++ == 1)
            put32 (p + 4, 32);
        p += n;
    }
    FlpImportOptions options;
    options.roots.factoryData = (std::filesystem::path (realFlpDir) / "..").string();
    options.readFile = [] (const std::string& path) {
        auto posix = path;
        std::replace (posix.begin(), posix.end(), '\\', '/');
        std::ifstream f (posix, std::ios::binary);
        return std::vector<std::uint8_t> ((std::istreambuf_iterator<char> (f)), std::istreambuf_iterator<char>());
    };
    const auto song = importFlp (flp::parseProject (b), options);
    CHECK (! song.channels.empty());
    if (song.channels.empty())
        return;
    const auto& c = song.channels[0];
    CHECK (std::abs (c.misc.shiftBeats - 22.0f / 96.0f) < 1e-6f);
    CHECK (std::abs (c.misc.gateBeats - 5.0f / 96.0f) < 1e-6f); // 600 → 5 ticks at 96 PPQ [EXEC]
    CHECK (! c.misc.fullPorta && ! c.misc.mainPitch && c.misc.pitchRange == 5);
    CHECK (std::abs (c.misc.echo.feedback - 0.5f) < 1e-6f && std::abs (c.misc.echo.pan + 0.5f) < 1e-6f && c.misc.echo.pitch == 100.0f);
    CHECK (c.misc.echo.echoes == 3 && std::abs (c.misc.echo.timeBeats - 0.5f) < 1e-6f && c.misc.echo.pingPong && ! c.misc.echo.fat);
    CHECK (std::abs (c.misc.key.pan - 0.5f) < 1e-6f && c.misc.key.mid == 60 && c.misc.velocity.mid == 100);
    CHECK (c.arp.direction == 5 && c.arp.range == 2 && c.arp.slide && c.arp.octaveRepeat == 2);
    CHECK (std::abs (c.arp.gate - 0.5f) < 1e-6f && std::abs (c.arp.stepBeats - (float) channelmisc::flTimeKnobBeats (896)) < 1e-6f);
    // chord 5 from the installation's Chords.map: minor
    if (std::filesystem::exists (std::filesystem::path (realFlpDir) / ".." / "System" / "Config" / "Chords.map"))
        CHECK ((c.arp.chord == std::vector<int> { 0, 3, 7 }));
}

TEST (rea146b_midi_import_selection)
{
    using namespace lynx;
    midiimport::ParsedMidi midi;
    midi.trackNames = { "Conductor", "Piano", "Drums" };
    auto ev = [&] (int track, std::int64_t tick, int status, int d1, int d2) {
        midi.events.push_back ({ track, tick, (std::uint8_t) status, (std::uint8_t) d1, (std::uint8_t) d2 });
    };
    ev (1, 0, 0x90, 60, 100);
    ev (1, 0, 0xB0, 7, 64);
    ev (1, 480, 0x90, 60, 0); // note off as a note on with velocity 0
    ev (1, 480, 0xB0, 7, 127);
    ev (1, 960, 0x91, 62, 90); // MIDI channel 2
    ev (1, 1200, 0x81, 62, 0);
    ev (2, 0, 0x99, 36, 120);
    ev (2, 240, 0x89, 36, 0);
    CHECK ((midiimport::tracksWithContent (midi) == std::vector<int> { 1, 2 }));

    midiimport::Options o;
    auto g = midiimport::groups (midi, o);
    CHECK (g.size() == 2 && g[0].name == "Piano" && g[1].name == "Drums" && g[0].notes.size() == 2);
    CHECK (g[0].notes[0].length == 480);
    // only track 2
    o.track = 2;
    g = midiimport::groups (midi, o);
    CHECK (g.size() == 1 && g[0].name == "Drums");
    // MIDI channel 2 filtered out
    o.track = -1;
    o.channels = (std::uint16_t) ~(1u << 1);
    g = midiimport::groups (midi, o);
    CHECK (g.size() == 2 && g[0].notes.size() == 1);
    // per MIDI channel
    o.channels = 0xFFFF;
    o.perTrack = false;
    g = midiimport::groups (midi, o);
    CHECK (g.size() == 3 && g[0].name == "MIDI channel 1" && g[2].name == "MIDI channel 10");
    // zero-velocity notes: the note on with velocity 0 is a note (ending at the next event of its key)
    o.perTrack = true;
    o.zeroVelocityNotes = true;
    g = midiimport::groups (midi, o);
    CHECK (g[0].notes.size() == 3 && g[0].notes[1].velocity == 0.0f && g[0].notes[0].length == 480);

    // controller 7 → the new channel's volume lane in the pattern
    o.zeroVelocityNotes = false;
    o.controller = 7;
    Song song = Song::makeDefault();
    const auto before = song.channels.size();
    std::string error;
    CHECK (midiimport::importInto (song, midi, o, "song", error));
    CHECK (song.channels.size() == before + 2 && song.patterns.back().notes.size() == 3);
    const auto& lanes = song.patterns.back().events;
    CHECK (lanes.size() == 1 && lanes[0].target.kind == AutomationTargetKind::channelVolume && lanes[0].target.index == (int) before);
    if (! lanes.empty())
        CHECK (lanes[0].events.size() == 2 && std::abs (lanes[0].events[1].value - 1.0f) < 1e-6f && lanes[0].events[1].tick == 480);

    // the piano roll's import into a channel: without Blend the channel's notes go first
    Song roll = Song::makeDefault();
    roll.patterns[0].notes = { { 0, 100, 0, 50, 0.5f }, { 0, 100, 1, 51, 0.5f } };
    o.controller = -1;
    o.track = 1;
    auto added = midiimport::importIntoChannel (roll, 0, midi, o, error);
    CHECK (added.size() == 2 && roll.patterns[0].notes.size() == 3);
    o.blend = true;
    added = midiimport::importIntoChannel (roll, 0, midi, o, error);
    CHECK (added.size() == 2 && roll.patterns[0].notes.size() == 5);
}
