// --- Fruity Video Player (core/VideoPlayer.h) - round 165g

#include "core/VideoPlayer.h"

TEST (video165_fl_state)
{
    using namespace lynx::video;
    // FL 2026's Default.fst (0xD5 payload, 37 bytes): version 3, flags 0x92, time display 0, 3 params (100, 0, 0),
    // no path, the window size 375 x 0
    const std::vector<std::uint8_t> fl = { 0x03, 0, 0, 0, 0x92, 0, 0, 0, 0, 0, 0, 0, 0x03, 0, 0, 0, 0x64, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                           0x00, 0x77, 0x01, 0, 0, 0, 0, 0, 0 };
    const auto s = read (fl);
    CHECK (s.has_value());
    if (! s)
        return;
    CHECK (s->keepAspect && s->enableAudio && s->slave && ! s->loop && ! s->ignoreTempo && ! s->dock && s->transport == 0);
    CHECK (s->volume == 100 && ! s->mute && ! s->play && s->path.empty() && s->width == 375 && s->height == 0);
    CHECK (s->timeDisplay == hms && s->delay == 0);
    // written back: FL's bytes, then LYNX's tail
    const auto back = write (*s);
    CHECK (back.size() == fl.size() + 16 && std::equal (fl.begin(), fl.end(), back.begin()));
    CHECK (read (back) == s);
    // a defaulted Settings is FL's default
    CHECK (Settings {} == *s);

    // a path (UTF-16, with a non-ASCII letter), times, every flag
    Settings v;
    v.path = "C:\\Users\\kauan\\V\xc3\xad" "deos\\clip.mp4";
    v.loop = true;
    v.keepAspect = false;
    v.transport = 2;
    v.enableAudio = false;
    v.ignoreTempo = true;
    v.slave = false;
    v.dock = true;
    v.timeDisplay = smpte5994drop;
    v.volume = 127;
    v.mute = true;
    v.play = true;
    v.width = 640;
    v.height = 360;
    v.start = 12345678;
    v.end = 98765432;
    v.delay = unitsPerSecond * 3;
    const auto bytes = write (v);
    CHECK (read (bytes) == v);
    // the path's length byte and characters as FL writes them (FUN_0071b530)
    CHECK (bytes[28] == 30 && bytes[29] == 'C' && bytes[30] == 0 && bytes[29 + 2 * 16] == 0xed && bytes[30 + 2 * 16] == 0);
    std::uint32_t flags = 0;
    std::memcpy (&flags, bytes.data() + 4, 4);
    CHECK (flags == (1u | 8u | 0x40u | 0x100u));
    // older states: the audio switch in bits 4|5, slaved by default before version 3
    auto old = fl;
    old[0] = 2;
    old[4] = 0x20;
    CHECK (! read (old).has_value() || read (old)->enableAudio);
    CHECK (! read ({ 9, 0, 0, 0 }).has_value() && ! read ({}).has_value());
}

TEST (video165_time_display)
{
    using namespace lynx::video;
    CHECK (formatTime (0.0, hms, 0.0) == "0:00:00:000");
    CHECK (formatTime (3723.456, hms, 0.0) == "1:02:03:456");
    CHECK (formatTime (-1.25, hms, 0.0) == "-0:00:01:250");
    CHECK (formatTime (1.52, smpte25, 0.0) == "0:00:01:13");
    CHECK (formatTime (59.99, smpte30, 0.0) == "0:00:59:29");
    // drop frame: 29.97 skips frames 0 and 1 at each minute but the tenth
    CHECK (formatTime (1800.0 / 29.97, smpte2997drop, 0.0) == "0:01:00;02");
    CHECK (formatTime (17982.0 / 29.97, smpte2997drop, 0.0) == "0:10:00;00");
    CHECK (formatTime (1800.0 / 29.97, smpte2997, 0.0) == "0:01:00:00");
    CHECK (formatTime (2.0, smpteAuto, 24.0) == "0:00:02:00" && formatTime (2.5, smpteAuto, 24.0) == "0:00:02:12");
    CHECK (timeDisplayFps (smpte23976, 0.0) == 23.976 && timeDisplayDrops (smpte5994drop) && ! timeDisplayDrops (smpte5994));
}

TEST (video165_clock_and_report)
{
    using namespace lynx::video;
    Settings s;
    s.delay = unitsPerSecond;            // starts when the song reaches 1 s
    s.start = 2 * unitsPerSecond;        // from the video's 2 s
    s.end = 5 * unitsPerSecond;          // to 5 s
    CHECK (Clock::videoTime (0.5, s, 10.0) < 0.0);
    CHECK (std::abs (Clock::videoTime (1.0, s, 10.0) - 2.0) < 1e-9 && std::abs (Clock::videoTime (3.5, s, 10.0) - 4.5) < 1e-9);
    CHECK (std::abs (Clock::videoTime (9.0, s, 10.0) - 5.0) < 1e-9); // holds the end
    s.loop = true;
    CHECK (std::abs (Clock::videoTime (5.0, s, 10.0) - 3.0) < 1e-9); // 4 s in = one 3-s pass + 1 s
    s.end = 0;                           // to the video's end
    s.loop = false;
    CHECK (std::abs (Clock::videoTime (20.0, s, 10.0) - 10.0) < 1e-9);

    const std::string report =
        "Input #0, mov,mp4,m4a,3gp,3g2,mj2, from 'clip.mp4':\n"
        "  Duration: 00:01:02.50, start: 0.000000, bitrate: 1205 kb/s\n"
        "  Stream #0:0[0x1](und): Video: h264 (High) (avc1 / 0x31637661), yuv420p(progressive), 1280x720 [SAR 1:1 DAR 16:9], 1072 kb/s, 29.97 fps, 29.97 tbr, 30k tbn (default)\n"
        "  Stream #0:1[0x2](und): Audio: aac (LC) (mp4a / 0x6134706D), 48000 Hz, stereo, fltp, 128 kb/s (default)\n";
    const auto info = parseFfmpegReport (report);
    CHECK (info.hasVideo && info.hasAudio && info.width == 1280 && info.height == 720);
    CHECK (std::abs (info.fps - 29.97) < 1e-9 && std::abs (info.duration - 62.5) < 1e-9);
    const auto none = parseFfmpegReport ("Input #0, wav, from 'a.wav':\n  Duration: 00:00:03.00, bitrate: 1411 kb/s\n  Stream #0:0: Audio: pcm_s16le, 44100 Hz\n");
    CHECK (! none.hasVideo && none.hasAudio);
}

TEST (video165_instrument_plays_the_track)
{
    using namespace lynx;
    // a 2-s ramp-like sound track at 48 kHz (left = sample index, right = minus it)
    auto track = std::make_shared<video::AudioTrack>();
    track->rate = 48000.0;
    for (int i = 0; i < 96000; ++i)
    {
        track->samples.push_back ((std::int16_t) (i % 30000));
        track->samples.push_back ((std::int16_t) -(i % 30000));
    }
    video::setAudioProvider ([track] (const std::string& path, bool) -> std::shared_ptr<const video::AudioTrack> {
        return path == "clip.mp4" ? track : nullptr;
    });
    auto inst = video::makeInstrument();
    CHECK (video::playerOf (inst.get()) != nullptr);
    ChannelDef def;
    def.instrument = InstrumentKind::videoplayer;
    video::Settings s;
    s.path = "clip.mp4";
    s.delay = video::unitsPerSecond / 2; // the video starts at 0.5 s of the song
    def.pluginState = video::write (s);
    const double bpm = 120.0, rate = 48000.0;
    inst->prepare (rate);
    inst->configure (def, bpm);
    // stopped: silent
    std::vector<float> l (480, 0.0f), r (480, 0.0f);
    inst->setSongPosition (0.0);
    inst->render (l.data(), r.data(), 480, 1.0f, 1.0f);
    CHECK (std::all_of (l.begin(), l.end(), [] (float x) { return x == 0.0f; }));
    // play from the song's 1.0 s: the video is at 0.5 s → frame 24000
    const auto ticksPerSample = bpm * ppq / (60.0 * rate);
    auto ticks = 1.0 * bpm / 60.0 * ppq;
    double at = ticks;
    inst->setSongPosition (at - 480 * ticksPerSample); // (the block before: it moves)
    inst->render (l.data(), r.data(), 480, 1.0f, 1.0f);
    std::fill (l.begin(), l.end(), 0.0f);
    std::fill (r.begin(), r.end(), 0.0f);
    inst->setSongPosition (at);
    inst->render (l.data(), r.data(), 480, 1.0f, 1.0f);
    CHECK (std::abs (l[0] * 32768.0f - 24000.0f) < 1.5f && std::abs (l[100] * 32768.0f - 24100.0f) < 1.5f);
    CHECK (std::abs (r[100] + l[100]) < 1e-6f);
    // muted / volume 50 %
    s.volume = 50;
    def.pluginState = video::write (s);
    inst->configure (def, bpm);
    std::fill (l.begin(), l.end(), 0.0f);
    at += 480 * ticksPerSample;
    inst->setSongPosition (at);
    inst->render (l.data(), r.data(), 480, 1.0f, 1.0f);
    CHECK (std::abs (l[0] * 32768.0f - 24480.0f * 0.5f) < 1.5f);
    s.mute = true;
    def.pluginState = video::write (s);
    inst->configure (def, bpm);
    CHECK (inst->idle());
    // before the delay: silence
    s.mute = false;
    def.pluginState = video::write (s);
    inst->configure (def, bpm);
    std::fill (l.begin(), l.end(), 0.0f);
    inst->setSongPosition (0.0);
    inst->setSongPosition (480 * ticksPerSample);
    inst->render (l.data(), r.data(), 480, 1.0f, 1.0f);
    CHECK (std::all_of (l.begin(), l.end(), [] (float x) { return x == 0.0f; }));
    video::setAudioProvider (nullptr);
}
