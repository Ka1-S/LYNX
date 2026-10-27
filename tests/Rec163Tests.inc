// --- Hardware recording: placement, latency ping, take stream, crash-safe WAV (core/TakeRecording.h) - round 163 ------

#include "core/TakeRecording.h"

#include <random>

namespace rec163
{
// the song tick the clip's first played sample came from (the audio skipped at the clip start included)
double clipAudioStart (std::int64_t start, std::int64_t offset, float fraction) { return (double) start - ((double) offset + (double) fraction); }

std::string tempPath (const std::string& name)
{
    std::error_code ec;
    return (std::filesystem::temp_directory_path (ec) / ("lynx_rec163_" + processTempTag() + "_" + name)).string();
}
} // namespace rec163

TEST (rec163_latency_total)
{
    lynx::takes::Latency l;
    l.driverInput = 100;
    l.driverOutput = 200;
    CHECK (l.roundTrip() == 300 && l.total() == 300);
    l.manual = -12;
    l.pdc = 64;
    CHECK (l.total() == 300 - 12 + 64);
    l.measured = 517; // a measured round trip replaces the driver's figures
    CHECK (l.roundTrip() == 517 && l.total() == 517 - 12 + 64);
    CHECK (lynx::takes::setupKey ("ASIO", "SSL 2", "SSL 2", 48000.0, 128) == "ASIO|SSL 2|SSL 2|48000|128");
}

TEST (rec163_placement_is_sample_accurate)
{
    using namespace lynx::takes;
    // 120 bpm, 48 kHz: 960 ticks per 24000 samples
    const auto tps = 960.0 / 24000.0;
    const auto p = place (1000.0, 100.0, tps, 240); // 240 samples = 9.6 ticks earlier
    CHECK (std::abs (p.exactStart - 990.4) < 1e-9);
    CHECK (p.start == 991 && p.offset == 1 && std::abs (p.offsetFraction - (-0.4f)) < 1e-6f);
    CHECK (std::abs (rec163::clipAudioStart (p.start, p.offset, p.offsetFraction) - 990.4) < 1e-6);
    CHECK (p.length == 99); // ends at 1090.4: whole ticks only

    // the take starts before the song: the clip starts at 0 and skips what came before
    const auto early = place (2.0, 500.0, tps, 480);
    CHECK (early.start == 0 && std::abs ((double) early.offset + (double) early.offsetFraction - 17.2) < 1e-5);

    // no audio, no clip
    CHECK (! place (10.0, 0.0, tps, 0).valid());

    // any position, tempo and latency: the clip's audio lines up with the song to well under a sample
    std::mt19937 rng (163);
    std::uniform_real_distribution<double> pos (0.0, 2.0e6), len (1.0, 5.0e5), bpm (40.0, 240.0);
    std::uniform_int_distribution<int> lat (0, 20000);
    auto worst = 0.0;
    for (int i = 0; i < 2000; ++i)
    {
        const auto rate = i % 2 == 0 ? 48000.0 : 44100.0;
        const auto t = bpm (rng) * 960.0 / (60.0 * rate);
        const auto q = place (pos (rng), len (rng), t, lat (rng));
        if (! q.valid())
            continue;
        const auto audio = std::max (0.0, q.exactStart);
        worst = std::max (worst, std::abs (rec163::clipAudioStart (q.start, q.offset, q.offsetFraction) - std::min (q.exactStart, audio)) / t);
        CHECK (q.offsetFraction >= -0.5f && q.offsetFraction <= 0.5f);
        CHECK ((double) (q.start + q.length) <= q.exactStart + q.ticks + 1e-6);
    }
    CHECK (worst < 0.01); // in samples
}

TEST (rec163_punch_parts_keep_alignment)
{
    using namespace lynx::takes;
    const auto p = place (1000.25, 4000.0, 0.04, 0);
    const auto parts = punch (p, { { 2000, 3000 }, { 4500, 9000 } });
    CHECK (parts.size() == 2);
    if (parts.size() == 2)
    {
        CHECK (parts[0].start == 2000 && parts[0].length == 1000);
        CHECK (parts[1].start == 4500 && parts[1].length == 5000 - 4500);
        for (const auto& part : parts) // each part plays the audio of its own song position
            CHECK (std::abs (rec163::clipAudioStart (part.start, part.offset, part.offsetFraction) - 1000.25) < 1e-5);
    }
    CHECK (punch (p, {}).size() == 1);
    CHECK (punch (p, { { 0, 900 } }).empty());
}

TEST (rec163_loop_passes)
{
    using namespace lynx::takes;
    // loop [1000, 2000), recording from the loop start for 2.5 passes, no latency
    const auto a = loopPasses (place (1000.0, 2500.0, 0.04, 0), 1000, 2000);
    CHECK (a.size() == 3);
    if (a.size() == 3)
    {
        CHECK (a[0].start == 1000 && a[0].length == 1000 && a[0].offset == 0);
        CHECK (a[1].start == 1000 && a[1].length == 1000 && a[1].offset == 1000);
        CHECK (a[2].start == 1000 && a[2].length == 500 && a[2].offset == 2000);
    }

    // latency of 10 ticks (250 samples): every pass's audio is 10 ticks later in the file
    const auto b = loopPasses (place (1000.0, 2500.0, 0.04, 250), 1000, 2000);
    CHECK (b.size() == 3);
    if (b.size() == 3)
    {
        CHECK (b[0].start == 1000 && b[0].offset == 10 && b[0].length == 1000);
        CHECK (b[1].start == 1000 && b[1].offset == 1010 && b[1].length == 1000);
        CHECK (b[2].start == 1000 && b[2].offset == 2010 && b[2].length == 490);
    }

    // a pre-roll before the loop is not part of the first pass; a short last pass is dropped
    const auto c = loopPasses (place (500.0, 2520.0, 0.04, 0), 1000, 2000, 100);
    CHECK (c.size() == 2);
    if (c.size() == 2)
        CHECK (c[0].start == 1000 && c[0].offset == 500 && c[1].offset == 1500);

    // the passes follow the transport's real wraps (a pass is a whole number of samples: slightly longer than L)
    {
        const auto tps = 0.0371;
        const auto samplesPerPass = std::ceil (1000.0 / tps);
        const std::vector<double> wraps { samplesPerPass * tps, 2.0 * samplesPerPass * tps };
        const auto e = loopPasses (place (1000.0, 2900.0, tps, 77), 1000, 2000, 0, wraps);
        CHECK (e.size() == 3);
        for (std::size_t k = 1; k < e.size(); ++k) // pass k's audio starts latency + wrap after the file's start
            CHECK (std::abs ((double) e[k].offset + (double) e[k].offsetFraction - (77 * tps + wraps[k - 1])) < 1e-6);
    }

    // fractional positions: each pass still lines up with its song position
    const auto d = loopPasses (place (1000.0, 3000.0, 0.0371, 77), 1000, 2000);
    for (std::size_t k = 0; k < d.size(); ++k)
    {
        const auto passStartInTake = (double) d[k].offset + (double) d[k].offsetFraction; // audio ticks before the clip start
        const auto exact = 1000.0 - 77 * 0.0371;
        // unwrapped tick of the clip's start = exact + skipped audio; it must be the pass start (+ the song tick)
        const auto unwrapped = exact + passStartInTake;
        const auto expected = k == 0 ? (double) d[k].start : 2000.0 + (double) (k - 1) * 1000.0 + ((double) d[k].start - 1000.0);
        CHECK (std::abs (unwrapped - expected) < 1e-5);
    }
}

TEST (rec163_latency_ping_finds_the_loopback)
{
    using namespace lynx::takes;
    // a loopback with `delay` samples, gain and polarity inversion, plus noise; blocks of 128
    const auto run = [] (int delay, float gain, float noiseLevel, int jitter) {
        LatencyPing ping;
        ping.start (8, 4800, 0.5f);
        std::vector<float> line ((std::size_t) (200000), 0.0f);
        std::mt19937 rng (7);
        std::uniform_real_distribution<float> noise (-noiseLevel, noiseLevel);
        std::uniform_int_distribution<int> jit (0, jitter);
        std::int64_t pos = 0;
        std::vector<float> out (128), in (128);
        int guard = 0;
        int extra = 0;
        while (ping.isRunning() && ++guard < 2000)
        {
            for (int i = 0; i < 128; ++i)
            {
                const auto at = pos + i - delay - extra;
                in[(std::size_t) i] = (at >= 0 ? -gain * line[(std::size_t) at] : 0.0f) + noise (rng);
            }
            std::fill (out.begin(), out.end(), 0.0f);
            float* outs[] = { out.data(), nullptr };
            ping.process (in.data(), outs, 2, 128);
            for (int i = 0; i < 128; ++i)
                line[(std::size_t) (pos + i)] = out[(std::size_t) i];
            pos += 128;
            if (jitter > 0 && pos % 4800 < 128)
                extra = jit (rng);
        }
        return ping.result();
    };

    const auto r = run (371, 0.1f, 1.0e-4f, 0);
    CHECK (r.ok && r.samples == 371 && r.spread == 0 && r.found == 8);
    CHECK (r.peak > 0.04f && r.peak < 0.06f);
    const auto block = run (128, 1.0f, 0.0f, 0); // one block (a driver that loops back on the next callback)
    CHECK (block.ok && block.samples == 128);
    const auto silent = run (371, 0.0f, 1.0e-4f, 0);
    CHECK (! silent.ok && silent.found == 0);
    const auto noisy = run (2000, 0.0005f, 0.01f, 0); // the click under the noise
    CHECK (! noisy.ok);
    const auto shaky = run (900, 0.5f, 0.0f, 40);
    CHECK (! shaky.ok && shaky.problem == "unstable");
}

TEST (rec163_take_stream_keeps_the_timeline)
{
    using namespace lynx::takes;
    // a counter pushed in random block sizes and pulled in random chunks comes out exactly
    {
        TakeStream s (2, 1 << 16);
        std::mt19937 rng (1);
        std::uniform_int_distribution<int> block (1, 700), chunk (1, 900);
        std::vector<float> l (1024), r (1024), ol (1024), orr (1024);
        std::int64_t pushed = 0, pulled = 0;
        auto exact = true;
        while (pushed < 400000)
        {
            const auto n = block (rng);
            for (int i = 0; i < n; ++i)
                l[(std::size_t) i] = (float) ((pushed + i) % 1000000), r[(std::size_t) i] = -l[(std::size_t) i];
            const float* d[] = { l.data(), r.data() };
            s.push (d, n);
            pushed += n;
            float* o[] = { ol.data(), orr.data() };
            const auto got = s.pull (o, chunk (rng));
            for (int i = 0; i < got; ++i)
                exact = exact && ol[(std::size_t) i] == (float) ((pulled + i) % 1000000) && orr[(std::size_t) i] == -ol[(std::size_t) i];
            pulled += got;
        }
        float* o[] = { ol.data(), orr.data() };
        for (int got; (got = s.pull (o, 1024)) > 0; pulled += got)
            for (int i = 0; i < got; ++i)
                exact = exact && ol[(std::size_t) i] == (float) ((pulled + i) % 1000000);
        CHECK (exact && pulled == pushed && s.lostFrames() == 0 && s.empty());
    }

    // the writer stalls: what didn't fit becomes silence at its place, everything after keeps its time
    {
        TakeStream s (1, 1024);
        std::vector<float> x (256);
        std::int64_t pushed = 0;
        const auto pushBlock = [&] {
            for (int i = 0; i < 256; ++i)
                x[(std::size_t) i] = (float) (pushed + i + 1);
            const float* d[] = { x.data() };
            s.push (d, 256);
            pushed += 256;
        };
        for (int b = 0; b < 10; ++b) // 2560 frames into 1024: 1536 lost
            pushBlock();
        CHECK (s.lostFrames() == 1536);
        std::vector<float> out (5000);
        float* o[] = { out.data() };
        auto got = s.pull (o, 5000);
        o[0] = out.data() + got;
        for (int b = 0; b < 4; ++b) // the writer is back
            pushBlock();
        s.finish();
        for (int g; (g = s.pull (o, 5000 - got)) > 0;)
        {
            // (pull writes at the start of `o`: shift the pointer)
            got += g;
            o[0] = out.data() + got;
        }
        CHECK (got == pushed);
        auto ok = true;
        for (std::int64_t i = 0; i < got; ++i)
        {
            const auto v = out[(std::size_t) i];
            const auto expectedValue = (float) (i + 1);
            const auto inGap = i >= 1024 && i < 2560;
            ok = ok && (inGap ? v == 0.0f : v == expectedValue);
        }
        CHECK (ok);
    }

    // frames lost at the very end of a take still count
    {
        TakeStream s (1, 1024);
        std::vector<float> x (2048, 1.0f);
        const float* d[] = { x.data() };
        s.push (d, 2048);
        s.finish();
        std::vector<float> out (4096);
        float* o[] = { out.data() };
        std::int64_t got = 0;
        for (int g; (g = s.pull (o, 4096 - (int) got)) > 0;)
            got += g, o[0] = out.data() + got;
        CHECK (got == 2048 && out[1023] == 1.0f && out[1024] == 0.0f && s.empty());
    }
}

TEST (rec163_wav_writer_formats_and_long_take)
{
    using namespace lynx::takes;
    const auto path = rec163::tempPath ("a.wav");
    // 24-bit stereo
    {
        WavTakeWriter w;
        CHECK (w.open (path, 48000.0, 2, SampleFormat::pcm24));
        std::vector<float> l { 0.0f, 0.5f, -0.5f, 1.0f, -1.0f, 1.5f }, r { 0.25f, -0.25f, 0.125f, 0.0f, 0.0f, -2.0f };
        const float* d[] = { l.data(), r.data() };
        CHECK (w.write (d, 6) && w.close());
        std::vector<std::vector<float>> back;
        double rate = 0.0;
        CHECK (readWav (path, back, &rate) && rate == 48000.0 && back.size() == 2 && back[0].size() == 6);
        if (back.size() == 2 && back[0].size() == 6)
        {
            auto worst = 0.0f;
            for (int i = 0; i < 5; ++i)
                worst = std::max ({ worst, std::abs (back[0][(std::size_t) i] - l[(std::size_t) i]), std::abs (back[1][(std::size_t) i] - r[(std::size_t) i]) });
            CHECK (worst < 2.0f / 8388607.0f);
            CHECK (back[0][5] == 1.0f && back[1][5] == -1.0f); // clipped, not wrapped
        }
    }

    // 32-bit float, a long take through the stream: bit-exact, sample count exact (no drift)
    {
        TakeStream s (1, 1 << 14);
        WavTakeWriter w;
        CHECK (w.open (path, 44100.0, 1, SampleFormat::float32, 0.5));
        const std::int64_t total = 44100LL * 120; // two minutes
        std::vector<float> block (64), chunk (8192);
        std::int64_t pushed = 0;
        std::mt19937 rng (3);
        std::uniform_int_distribution<int> pace (0, 3);
        while (pushed < total)
        {
            const auto n = (int) std::min<std::int64_t> (64, total - pushed);
            for (int i = 0; i < n; ++i)
                block[(std::size_t) i] = (float) ((pushed + i) % 16777216) * (1.0f / 16777216.0f);
            const float* d[] = { block.data() };
            s.push (d, n);
            pushed += n;
            if (pace (rng) == 0) // the writer wakes now and then
            {
                float* o[] = { chunk.data() };
                const auto got = s.pull (o, (int) chunk.size());
                const float* c[] = { chunk.data() };
                w.write (c, got);
            }
        }
        s.finish();
        for (;;)
        {
            float* o[] = { chunk.data() };
            const auto got = s.pull (o, (int) chunk.size());
            if (got == 0)
                break;
            const float* c[] = { chunk.data() };
            w.write (c, got);
        }
        CHECK (s.lostFrames() == 0 && w.frames() == total && w.close());
        std::vector<std::vector<float>> back;
        CHECK (readWav (path, back) && back.size() == 1 && (std::int64_t) back[0].size() == total);
        auto exact = back.size() == 1 && (std::int64_t) back[0].size() == total;
        for (std::int64_t i = 0; exact && i < total; ++i)
            exact = back[0][(std::size_t) i] == (float) (i % 16777216) * (1.0f / 16777216.0f);
        CHECK (exact);
    }
    std::error_code ec;
    std::filesystem::remove (path, ec);
}

TEST (rec163_crash_leaves_a_repairable_wav)
{
    using namespace lynx::takes;
    const auto path = rec163::tempPath ("crash.wav");
    WavTakeWriter w;
    CHECK (w.open (path, 48000.0, 2, SampleFormat::pcm24, 0.1)); // header every 4800 frames
    std::vector<float> l (1000, 0.25f), r (1000, -0.25f);
    const float* d[] = { l.data(), r.data() };
    for (int i = 0; i < 12; ++i)
        w.write (d, 1000);
    w.abandon(); // the process died: the header says 10000 frames (the last sync), the file holds 12000

    std::vector<std::vector<float>> before;
    CHECK (readWav (path, before) && before.size() == 2 && before[0].size() == 10000); // already playable
    std::int64_t frames = 0;
    bool changed = false;
    CHECK (repairWav (path, &frames, &changed) && changed && frames == 12000);
    std::vector<std::vector<float>> after;
    CHECK (readWav (path, after) && after.size() == 2 && after[0].size() == 12000 && std::abs (after[1][11999] + 0.25f) < 1e-6f);
    CHECK (repairWav (path, &frames, &changed) && ! changed); // a good file stays as it is

    // a file cut in the middle of a frame (the last bytes never reached the disk)
    {
        std::ofstream f (path, std::ios::binary | std::ios::app);
        f.write ("\x01\x02", 2);
    }
    CHECK (repairWav (path, &frames, &changed) && frames == 12000);
    CHECK (! repairWav (rec163::tempPath ("missing.wav")));
    std::error_code ec;
    std::filesystem::remove (path, ec);
}

TEST (rec163_journal_names_disk)
{
    using namespace lynx::takes;
    Journal j;
    j.wav = "C:/Music/LYNX/Recordings/Bass DI_Take3.wav";
    j.firstTick = 15360.123456789;
    j.ticksPerSample = 0.04;
    j.sampleRate = 48000.0;
    j.latency = 517;
    j.insert = 3;
    j.playlistTrack = 7;
    j.project = "C:/Projects/song.lynx";
    Journal k;
    CHECK (parseJournal (journalText (j), k));
    CHECK (k.wav == j.wav && k.firstTick == j.firstTick && k.ticksPerSample == j.ticksPerSample && k.latency == 517 && k.insert == 3
           && k.playlistTrack == 7 && k.project == j.project && k.sampleRate == 48000.0);
    CHECK (! parseJournal ("garbage", k));

    CHECK (takeName ("Bass DI", 3) == "Bass DI_Take3");
    CHECK (takeName ("", 1) == "Audio_Take1");
    CHECK (takeName ("Gtr: L/R?", 2) == "Gtr_ L_R__Take2");
    CHECK (takeName ("Vox. ", 0) == "Vox_Take1");

    // 1 GB free, 48 kHz stereo 24-bit: 1e9 / 288000 B/s = 57.9 minutes
    CHECK (std::abs (minutesLeft (1000000000LL, 48000.0, 2, SampleFormat::pcm24) - 57.87) < 0.01);
    CHECK (minutesLeft (-5, 48000.0, 2, SampleFormat::float32) == 0.0);
}

TEST (rec163_take_lanes_and_comping)
{
    using namespace lynx;
    using namespace lynx::takes;
    auto song = Song::makeDefault();
    song.mode = PlayMode::song;
    song.playlist.clear();
    while (song.mixer.size() < 4)
        song.mixer.emplace_back();
    song.mixer[2].name = "Gtr";
    song.loopStart = 1000;
    song.loopEnd = 2000;

    // three passes of a loop recording on insert 2
    const auto parts = loopPasses (place (1000.0, 2900.0, 0.04, 0), 1000, 2000);
    CHECK (parts.size() == 3);
    placeTakeClips (song, { { "/rec/Gtr_Take1.wav", "Gtr_Take1", 2 } }, parts);
    CHECK (song.playlist.size() == 3 && song.channels.back().mixerTrack == 2);
    int top = -1;
    for (const auto& c : song.playlist)
        if (! c.muted)
            top = c.track;
    CHECK (top == 0);                                                     // the last pass plays on the take's track
    CHECK (song.playlistTracks.size() >= 3 && song.playlistTracks[0].mode == PlaylistTrackMode::audio && song.playlistTracks[0].mixerTrack == 2
           && song.playlistTracks[0].name == "Gtr" && song.playlistTracks[0].collapsed);
    CHECK (song.playlistTracks.size() >= 3 && song.playlistTracks[1].grouped && song.playlistTracks[2].grouped); // lanes folded under it
    CHECK (compTrackOf (song, 2) == 0 && compTrackOf (song, 0) == -1);
    const auto lastPass = std::find_if (song.playlist.begin(), song.playlist.end(), [] (const Clip& c) { return ! c.muted; });
    CHECK (lastPass != song.playlist.end() && lastPass->offset == 2000); // newest pass on top

    // comp: the first pass's middle onto the comp track
    int firstPass = -1;
    for (int i = 0; i < (int) song.playlist.size(); ++i)
        if (song.playlist[(std::size_t) i].offset == 0)
            firstPass = i;
    CHECK (firstPass >= 0 && song.playlist[(std::size_t) firstPass].muted && song.playlist[(std::size_t) firstPass].track == 2);
    CHECK (promoteTake (song, firstPass, 0, 1200, 1500));
    std::vector<Clip> comp;
    for (const auto& c : song.playlist)
        if (c.track == 0)
            comp.push_back (c);
    std::sort (comp.begin(), comp.end(), [] (const Clip& a, const Clip& b) { return a.start < b.start; });
    // last pass 1000-1200, first pass 1200-1500, last pass 1500-1900 (the last pass is 900 ticks)
    CHECK (comp.size() == 3);
    if (comp.size() == 3)
    {
        CHECK (comp[0].start == 1000 && comp[0].length == 200 && comp[0].offset == 2000 && comp[0].fadeOut == compFadeTicks);
        CHECK (comp[1].start == 1200 && comp[1].length == 300 && comp[1].offset == 200 && ! comp[1].muted && comp[1].fadeIn == compFadeTicks);
        CHECK (comp[2].start == 1500 && comp[2].length == 400 && comp[2].offset == 2500 && comp[2].fadeIn == compFadeTicks);
    }
    CHECK (! promoteTake (song, firstPass, 0, 5000, 6000)); // outside the take

    // a take on the same insert later lands on the linked track when it is free there
    CHECK (takeTrack (song, 2, 4000, 5000) == 0);
    CHECK (takeTrack (song, 2, 1000, 1100) == 3); // busy: a new track after the lanes
    CHECK (takeTrack (song, 3, 4000, 5000) == 3); // another insert: not the Gtr track
}
