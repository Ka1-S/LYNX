// Round 165f (rea165-midisync): MIDI clock / MTC master sync and external clock sync
// (docs/analise/REA_rodada165f-midisync.md)

namespace midisync165
{
using namespace lynx::midisync;

// runs the generator over `blocks` blocks of `n` samples at `bpm`, starting at `startTicks`
struct Run
{
    std::vector<std::pair<std::int64_t, Message>> sent; // absolute sample, message
};

inline void play (Generator& g, Run& run, double& ticks, std::int64_t& sample, int blocks, int n, double bpm, double rate = 48000.0,
                  bool playing = true, bool jumped = false)
{
    for (int b = 0; b < blocks; ++b)
    {
        Block block;
        block.startTicks = ticks;
        block.ticksPerSample = bpm * 960.0 / (60.0 * rate);
        block.sampleRate = rate;
        block.numSamples = n;
        block.playing = playing;
        block.jumped = jumped && b == 0;
        BlockMessages out;
        g.process (block, ticks / (block.ticksPerSample * rate), out);
        for (int i = 0; i < out.count; ++i)
            run.sent.push_back ({ sample + out.items[(std::size_t) i].sample, out.items[(std::size_t) i] });
        if (playing)
            ticks += n * block.ticksPerSample;
        sample += n;
    }
}

inline int countStatus (const Run& r, std::uint8_t status)
{
    return (int) std::count_if (r.sent.begin(), r.sent.end(), [status] (const auto& m) { return m.second.bytes[0] == status; });
}
} // namespace midisync165

TEST (midisync165_clock_pulses_land_on_their_samples)
{
    using namespace midisync165;
    Generator g;
    Run run;
    double ticks = 0.0;
    std::int64_t sample = 0;
    // 120 BPM at 48 kHz: a pulse every 48000 * 0.5 / 24 = 1000 samples; blocks of 333 split pulses everywhere
    play (g, run, ticks, sample, 300, 333, 120.0);
    CHECK (! run.sent.empty() && run.sent[0].second.bytes[0] == 0xfa && run.sent[0].first == 0);
    std::vector<std::int64_t> pulses;
    for (const auto& [at, m] : run.sent)
        if (m.bytes[0] == 0xf8)
            pulses.push_back (at);
    CHECK (pulses.size() == 100); // 99 900 samples: pulses at 0, 1000 … 99 000
    for (std::size_t i = 0; i < pulses.size(); ++i)
        CHECK (pulses[i] == (std::int64_t) i * 1000);
    // stop: one Stop, then nothing
    play (g, run, ticks, sample, 10, 333, 120.0, 48000.0, false);
    CHECK (countStatus (run, 0xfc) == 1);
    CHECK (run.sent.back().second.bytes[0] == 0xfc);
}

TEST (midisync165_clock_follows_tempo_changes)
{
    using namespace midisync165;
    Generator g;
    Run run;
    double ticks = 0.0;
    std::int64_t sample = 0;
    play (g, run, ticks, sample, 48, 1000, 120.0); // 1 s at 120: 48 pulses, 1000 samples apart
    const auto before = (std::size_t) countStatus (run, 0xf8);
    play (g, run, ticks, sample, 48, 1000, 60.0); // 1 s at 60: 24 pulses, 2000 samples apart
    CHECK (before == 48);
    CHECK (countStatus (run, 0xf8) == 72);
    std::vector<std::int64_t> pulses;
    for (const auto& [at, m] : run.sent)
        if (m.bytes[0] == 0xf8)
            pulses.push_back (at);
    CHECK (pulses[48] == 48000 && pulses[49] == 50000 && pulses.back() == 94000);
}

TEST (midisync165_spp_and_continue_on_start_elsewhere_and_jumps)
{
    using namespace midisync165;
    CHECK (sppFor (0.0) == 0 && sppFor (240.0) == 1 && sppFor (241.0) == 2 && sppFor (3840.0) == 16);
    Generator g;
    Run run;
    double ticks = 3840.0 + 100.0; // bar 2 + a bit: SPP 17 (the next 16th), first pulse at tick 4080
    std::int64_t sample = 0;
    play (g, run, ticks, sample, 4, 1000, 120.0);
    CHECK (run.sent[0].second.bytes[0] == 0xf2 && run.sent[0].second.bytes[1] == 17 && run.sent[0].second.bytes[2] == 0);
    CHECK (run.sent[1].second.bytes[0] == 0xfb);
    CHECK (run.sent[2].second.bytes[0] == 0xf8);
    // 140 ticks to the pulse at 1 tick / 25 samples (120 BPM, 48 kHz: 0.04 ticks per sample)
    CHECK (run.sent[2].first == 3500);
    // a jump while playing (the loop goes back to 0): Stop, Start, pulse at once
    run.sent.clear();
    ticks = 0.0;
    play (g, run, ticks, sample, 1, 1000, 120.0, 48000.0, true, true);
    CHECK (run.sent.size() >= 3 && run.sent[0].second.bytes[0] == 0xfc && run.sent[1].second.bytes[0] == 0xfa && run.sent[2].second.bytes[0] == 0xf8);
    // a big SPP: 14 bits, LSB first
    run.sent.clear();
    ticks = 240.0 * 1000.0;
    play (g, run, ticks, sample, 1, 10, 120.0, 48000.0, true, true);
    CHECK (run.sent[1].second.bytes[0] == 0xf2 && run.sent[1].second.bytes[1] == (1000 & 0x7f) && run.sent[1].second.bytes[2] == (1000 >> 7));
}

TEST (midisync165_mtc_timecode_labels_drop_frame)
{
    using namespace lynx::midisync;
    const auto df = mtcRate (SyncType::mtc2997Drop);
    CHECK (df.code == 2 && df.drop && df.nominal == 30);
    // the first minute has every label; minute 1 starts at 00:01:00;02
    CHECK ((frameToTimecode (1799, df) == Timecode { 0, 0, 59, 29 }));
    CHECK ((frameToTimecode (1800, df) == Timecode { 0, 1, 0, 2 }));
    // minute 10 keeps its 0 and 1
    CHECK ((frameToTimecode (17982, df) == Timecode { 0, 10, 0, 0 }));
    CHECK ((frameToTimecode (17981, df) == Timecode { 0, 9, 59, 29 }));
    // one hour of 29.97 drop = 107 892 frames = 01:00:00;00 exactly
    CHECK ((frameToTimecode (107892, df) == Timecode { 1, 0, 0, 0 }));
    for (std::int64_t f : { 0, 1, 2, 1799, 1800, 1801, 3597, 3598, 17981, 17982, 17983, 107891, 107892, 500000 })
        CHECK (timecodeToFrame (frameToTimecode (f, df), df) == f);
    // non-drop 29.97: plain 30-frame labels (it drifts from the clock by design)
    const auto nd = mtcRate (SyncType::mtc2997);
    CHECK (nd.code == 3 && ! nd.drop);
    CHECK ((frameToTimecode (1800, nd) == Timecode { 0, 1, 0, 0 }));
    const auto f24 = mtcRate (SyncType::mtc24), f25 = mtcRate (SyncType::mtc25), f30 = mtcRate (SyncType::mtc30), d30 = mtcRate (SyncType::mtc30Drop);
    CHECK (f24.code == 0 && f25.code == 1 && f30.code == 3 && d30.code == 2 && d30.fps == 30.0);
    CHECK ((frameToTimecode (24 * 3661 + 5, f24) == Timecode { 1, 1, 1, 5 }));
    CHECK ((frameToTimecode (25 * 59 + 24, f25) == Timecode { 0, 0, 59, 24 }));
}

TEST (midisync165_mtc_quarter_frames_and_full_frame)
{
    using namespace midisync165;
    // the pieces of 01:23:45:12 at 25 fps
    const Timecode tc { 1, 23, 45, 12 };
    const std::uint8_t expect[8] { 0x0c, 0x10, 0x2d, 0x32, 0x47, 0x51, 0x61, 0x72 };
    for (int p = 0; p < 8; ++p)
        CHECK (quarterFrameData (tc, p, 1) == expect[p]);
    std::uint8_t ff[10] {};
    fullFrame (tc, 1, ff);
    const std::uint8_t ffExpect[10] { 0xf0, 0x7f, 0x7f, 0x01, 0x01, 0x21, 23, 45, 12, 0xf7 };
    CHECK (std::equal (ff, ff + 10, ffExpect));

    for (int t = 1; t < syncTypeCount; ++t)
    {
        const auto type = (SyncType) t;
        const auto rate = mtcRate (type);
        Generator g;
        g.setType (type);
        Run run;
        double ticks = 0.0;
        std::int64_t sample = 0;
        play (g, run, ticks, sample, 100, 480, 120.0); // 1 s
        // a full frame at the start, then 4 quarter frames per frame, in piece order 0…7
        CHECK (run.sent[0].second.size == 10 && run.sent[0].second.bytes[5] == (std::uint8_t) (rate.code << 5));
        std::vector<std::pair<std::int64_t, std::uint8_t>> qf;
        for (const auto& [at, m] : run.sent)
            if (m.bytes[0] == 0xf1)
                qf.push_back ({ at, m.bytes[1] });
        const auto expected = (std::size_t) std::ceil (rate.fps * 4.0 - 1.0e-9);
        CHECK (qf.size() == expected);
        for (std::size_t i = 0; i < qf.size(); ++i)
        {
            CHECK ((qf[i].second >> 4) == (int) (i % 8));
            CHECK (std::llabs (qf[i].first - std::llround (i * 48000.0 / (rate.fps * 4.0))) <= 1);
        }
        // the second sequence (frames 2–3) carries frame 2
        CHECK ((qf[8].second & 0x0f) == 2 && (qf[9].second & 0x0f) == 0);
        CHECK ((qf[7].second & 0x06) >> 1 == rate.code);
    }

    // a locate at 10 s (25 fps): a full frame 00:00:10:00, then quarter frames from frame 250
    Generator g;
    g.setType (SyncType::mtc25);
    Run run;
    double ticks = 10.0 * 2.0 * 960.0; // 10 s at 120 BPM
    std::int64_t sample = 0;
    play (g, run, ticks, sample, 10, 480, 120.0);
    CHECK (run.sent[0].second.size == 10 && run.sent[0].second.bytes[7] == 10 && run.sent[0].second.bytes[8] == 0);
    CHECK (run.sent[1].second.bytes[0] == 0xf1 && run.sent[1].first == 0 && run.sent[1].second.bytes[1] == (0x00 | (250 % 25)));
    CHECK (run.sent[3].second.bytes[1] == (0x20 | 10)); // seconds low nibble
}

TEST (midisync165_receiver_tempo_from_jittery_clock)
{
    using namespace lynx::midisync;
    std::mt19937 random (165);
    std::uniform_real_distribution<double> jitter (-0.001, 0.001); // ±1 ms per message (a USB MIDI interface)
    for (const auto bpm : { 60.0, 98.5, 120.0, 174.0, 240.0 })
    {
        Receiver r;
        const std::uint8_t clock = 0xf8;
        const auto period = 60.0 / (bpm * 24.0);
        double shown = 0.0, maxSwing = 0.0;
        for (int i = 0; i < 24 * 16; ++i)
        {
            r.feed (&clock, 1, 10.0 + i * period + jitter (random));
            if (i >= 24 * 4)
            {
                if (shown > 0.0)
                    maxSwing = std::max (maxSwing, std::abs (r.bpm() - shown));
                shown = r.bpm();
            }
        }
        CHECK (std::abs (r.bpm() - bpm) < 0.15 * bpm / 120.0 + 0.1);
        CHECK (maxSwing < 0.3); // the hysteresis keeps the readout steady
        CHECK (r.receiving (10.0 + 24 * 16 * period) && ! r.receiving (20.0 + 24 * 16 * period));
    }
    // a tempo change is followed within a beat
    Receiver r;
    const std::uint8_t clock = 0xf8;
    double t = 0.0;
    for (int i = 0; i < 96; ++i, t += 60.0 / (120.0 * 24.0))
        r.feed (&clock, 1, t);
    for (int i = 0; i < 24; ++i, t += 60.0 / (90.0 * 24.0))
        r.feed (&clock, 1, t);
    CHECK (std::abs (r.bpm() - 90.0) < 0.2);
    // one late message does not restart the fit
    Receiver q;
    t = 0.0;
    for (int i = 0; i < 96; ++i, t += 60.0 / (120.0 * 24.0))
        q.feed (&clock, 1, t + (i == 60 ? 0.008 : 0.0));
    CHECK (std::abs (q.bpm() - 120.0) < 0.3);
    // a clock too fast for a tempo
    Receiver f;
    for (int i = 0; i < 48; ++i)
        f.feed (&clock, 1, i * 0.0001);
    CHECK (f.tooFast() && statusText (f, 0.0047).find ("too fast") != std::string::npos);
    CHECK (statusText (Receiver(), 0.0) == "No clock");
}

TEST (midisync165_receiver_transport_commands_and_position)
{
    using namespace lynx::midisync;
    Receiver r;
    const std::uint8_t clock = 0xf8, start = 0xfa, stop = 0xfc, resume = 0xfb;
    const std::uint8_t spp[3] { 0xf2, 0x10, 0x01 }; // 16 + 128 = 144 sixteenths = 36 beats
    const auto period = 60.0 / (120.0 * 24.0);
    Receiver::Command c;
    double ticks = -1.0;
    // a clock while stopped: the tempo is known, the song does not move
    for (int i = 0; i < 30; ++i)
        r.feed (&clock, 1, i * period);
    CHECK (! r.running() && std::abs (r.bpm() - 120.0) < 0.05 && ! r.popCommand (c, ticks));
    CHECK (r.feed (&start, 1, 30 * period - 0.001));
    CHECK (r.popCommand (c, ticks) && c == Receiver::Command::start && ticks == 0.0);
    CHECK (! r.running()); // starts on the next pulse
    r.feed (&clock, 1, 30 * period);
    CHECK (r.running() && r.ticksAt (30 * period) == 0.0);
    CHECK (std::abs (r.ticksAt (30 * period + period / 2) - 20.0) < 1.0e-6);
    CHECK (std::abs (r.ticksAt (30 * period + 3 * period) - 120.0) < 1.0e-6); // extrapolated…
    CHECK (std::abs (r.ticksAt (30 * period + 5.0) - 40.0 * 0.1 / period) < 1.0e-6); // …at most 0.1 s
    for (int i = 31; i < 30 + 49; ++i)
        r.feed (&clock, 1, i * period);
    CHECK (std::abs (r.ticksAt (78 * period) - 48 * 40.0) < 1.0e-6); // two beats in
    r.feed (&stop, 1, 79 * period);
    CHECK (r.popCommand (c, ticks) && c == Receiver::Command::stop && ! r.running());
    // SPP then Continue: playback resumes at bar 10 on the next pulse
    CHECK (r.feed (spp, 3, 80 * period));
    CHECK (r.popCommand (c, ticks) && c == Receiver::Command::locate && ticks == 144 * 240.0);
    r.feed (&resume, 1, 81 * period);
    CHECK (r.popCommand (c, ticks) && c == Receiver::Command::resume && ticks == 144 * 240.0);
    r.feed (&clock, 1, 82 * period);
    r.feed (&clock, 1, 83 * period);
    CHECK (r.running() && r.ticksAt (83 * period) == 144 * 240.0 + 40.0);
    // other messages are not sync
    const std::uint8_t note[3] { 0x90, 60, 100 };
    CHECK (! r.feed (note, 3, 0.0));
}

TEST (midisync165_generator_feeds_receiver_loopback)
{
    // the self-check: what the master sends, a receiver follows (tempo, start, position)
    using namespace midisync165;
    Generator g;
    Receiver r;
    Run run;
    double ticks = 0.0;
    std::int64_t sample = 0;
    play (g, run, ticks, sample, 200, 256, 133.0, 44100.0);
    for (const auto& [at, m] : run.sent)
        r.feed (m.bytes.data(), m.size, at / 44100.0);
    CHECK (std::abs (r.bpm() - 133.0) < 0.05);
    CHECK (r.running());
    const auto now = sample / 44100.0;
    CHECK (std::abs (r.ticksAt (now) - ticks) <= 40.0);
    Receiver::Command c;
    double at = 0.0;
    CHECK (r.popCommand (c, at) && c == Receiver::Command::start);
}

TEST (midisync165_settings_round_trip)
{
    lynx::prefs::MidiPrefs p;
    lynx::prefs::MidiOutputPrefs o;
    o.name = "Volca | Sample";
    o.masterSync = true;
    o.syncType = "mtc2997drop";
    o.offsetMs = -12;
    o.allNotesOff = true;
    p.setOutput (o);
    p.clockInput = "TR-8S";
    p.clockOffsetMs = 7;
    const auto back = lynx::prefs::MidiPrefs::fromText (p.toText());
    CHECK (back == p);
    CHECK (back.output ("Volca | Sample").syncType == "mtc2997drop");
    CHECK (! back.output ("other").masterSync && back.output ("other").syncType == "clock");
    // defaults are not stored
    lynx::prefs::MidiOutputPrefs plain;
    plain.name = "x";
    p.setOutput (plain);
    CHECK (p.outputs.size() == 1);
    CHECK (lynx::midisync::syncTypeFromKey ("bogus") == lynx::midisync::SyncType::midiClock);
    for (int t = 0; t < lynx::midisync::syncTypeCount; ++t)
        CHECK (lynx::midisync::syncTypeFromKey (lynx::midisync::syncTypeKey ((lynx::midisync::SyncType) t)) == (lynx::midisync::SyncType) t);
}
