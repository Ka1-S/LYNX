// --- Round 163c (user feedback: "pitching didn't work for me"): audio clip pitch, keep length, the Pitch... command ----

namespace w163c
{
// An audio clip of `sample` (a 2-bar clip at bar 0, 120 BPM) on a channel with `pitch` semitones, `fine` cents and
// "keep length"; rendered in song mode for `seconds` (the clip itself lasts 4 s). Returns the left channel.
std::vector<float> renderClip (std::shared_ptr<lynx::SampleData> sample, float pitch, float fine, bool keepLength, double seconds = 3.0,
                               std::int64_t clipLength = 0)
{
    using namespace lynx;
    Song s;
    s.mixer = Song::defaultMixer (2);
    s.bpm = 120.0;
    s.mode = PlayMode::song;
    ChannelDef c;
    c.instrument = InstrumentKind::audioClip;
    c.samplePath = "sine.wav";
    c.sampler.pitch = pitch;
    c.sampler.keepPitch = keepLength;
    c.misc.fineTune = fine;
    c.mixerTrack = 1;
    s.channels.push_back (c);
    s.patterns.emplace_back();
    Clip clip;
    clip.kind = ClipKind::audio;
    clip.channel = 0;
    clip.start = 0;
    clip.length = clipLength > 0 ? clipLength : (std::int64_t) ppq * 4 * 4; // 4 bars = 8 s at 120 BPM
    s.playlist.push_back (clip);
    RackState::Factories f;
    f.samples = [sample] (const std::string&) { return std::shared_ptr<const SampleData> (sample); };
    constexpr double rate = 44100.0;
    auto state = RackState::build (s, nullptr, rate, f);
    RackRenderer r;
    r.prepare (rate, 441);
    std::vector<float> out, l (441), rr (441);
    double tick = 0.0;
    for (int b = 0; b < (int) (seconds * 100.0); ++b)
    {
        r.process (state.get(), true, tick, 441, l.data(), rr.data());
        tick += 441 * ticksPerSample (s.bpm, rate);
        out.insert (out.end(), l.begin(), l.end());
    }
    return out;
}

double cents (double heard, double expected)
{
    return heard > 0.0 && expected > 0.0 ? 1200.0 * std::log2 (heard / expected) : 1.0e9;
}
} // namespace w163c

TEST (w163c_an_audio_clip_follows_its_channel_pitch_by_resampling)
{
    // 1 s of 440 Hz: the Clip properties' / Channel settings' pitch did nothing to audio clips
    const auto sine = s162::sine (440.0, 1.0);
    const auto up = s162::analyse (w163c::renderClip (sine, 12.0f, 0.0f, false), 44100.0);
    CHECK (std::abs (w163c::cents (up.freq, 880.0)) < 2.0);
    CHECK (std::abs (up.length - 22050) < 300); // like a tape: an octave up plays in half the time
    const auto fifth = s162::analyse (w163c::renderClip (sine, 7.0f, 0.0f, false), 44100.0);
    CHECK (std::abs (w163c::cents (fifth.freq, 440.0 * std::exp2 (7.0 / 12.0))) < 2.0);
    const auto down = s162::analyse (w163c::renderClip (sine, -12.0f, 0.0f, false), 44100.0);
    CHECK (std::abs (w163c::cents (down.freq, 220.0)) < 2.0);
    CHECK (std::abs (down.length - 88200) < 300);
    // 50 cents: on the pitch knob (fractions of a semitone) or in Misc › Fine tune
    const auto half = s162::analyse (w163c::renderClip (sine, 0.5f, 0.0f, false), 44100.0);
    CHECK (std::abs (w163c::cents (half.freq, 440.0 * std::exp2 (0.5 / 12.0))) < 2.0);
    const auto fine = s162::analyse (w163c::renderClip (sine, 0.0f, 50.0f, false), 44100.0);
    CHECK (std::abs (w163c::cents (fine.freq, 440.0 * std::exp2 (0.5 / 12.0))) < 2.0);
}

TEST (w163c_keep_length_transposes_an_audio_clip_without_changing_its_length)
{
    const auto sine = s162::sine (440.0, 1.0);
    for (const auto semitones : { 7.0f, -12.0f, 0.5f })
    {
        const auto heard = s162::analyse (w163c::renderClip (sine, semitones, 0.0f, true), 44100.0);
        CHECK (std::abs (w163c::cents (heard.freq, 440.0 * std::exp2 (semitones / 12.0))) < 3.0);
        CHECK (std::abs (heard.length - 44100) < 2205); // within 50 ms of the second it lasts
    }
}

TEST (w163c_an_unpitched_clip_plays_exactly_as_before)
{
    // pitch 0: the same samples with or without "keep length" (no shifter, no resampling), at the sample's rate
    const auto sine = s162::sine (440.0, 1.0);
    const auto plain = w163c::renderClip (sine, 0.0f, 0.0f, false, 1.5);
    const auto kept = w163c::renderClip (sine, 0.0f, 0.0f, true, 1.5);
    CHECK (plain == kept);
    const auto heard = s162::analyse (plain, 44100.0);
    CHECK (std::abs (w163c::cents (heard.freq, 440.0)) < 1.0 && std::abs (heard.length - 44100) < 50);
}

TEST (w163c_a_sampler_keeping_length_transposes_its_keys_without_stretch)
{
    // "Keep length" with no Stretch: the keys transpose through the pitch shifter, the sample keeps its second
    lynx::SamplerSettings settings;
    settings.keepPitch = true;
    const auto sine = s162::sine (440.0, 1.0);
    const auto low = s162::playLive (settings, sine, 48, 0.0f, 2.0);
    CHECK (std::abs (w163c::cents (low.freq, 220.0)) < 3.0);
    CHECK (std::abs (low.length - 44100) < 2205);
    settings.keepPitch = false; // resampled: an octave down lasts twice as long
    const auto tape = s162::playLive (settings, sine, 48, 0.0f, 3.0);
    CHECK (std::abs (w163c::cents (tape.freq, 220.0)) < 2.0);
    CHECK (std::abs (tape.length - 88200) < 300);
}

TEST (w163c_the_pitch_command_sets_the_channel_and_rescales_its_clips)
{
    using namespace lynx;
    Song s;
    s.bpm = 120.0;
    ChannelDef c;
    c.instrument = InstrumentKind::audioClip;
    c.samplePath = "x.wav";
    s.channels.push_back (c);
    Clip clip;
    clip.kind = ClipKind::audio;
    clip.channel = 0;
    clip.length = (std::int64_t) ppq * 4; // 2 s at 120 BPM = the whole 2 s file
    clip.offset = (std::int64_t) ppq;
    s.playlist.push_back (clip);
    const double frames = 88200.0, rate = 44100.0;
    CHECK (clipops::fileTicks (s.channels[0], frames, rate, s.bpm) == (std::int64_t) ppq * 4);

    // an octave up, resampled: the file (and every clip of it) plays in half the time
    clipops::setChannelPitch (s, 0, 12.0, 0.0, false, frames, rate);
    CHECK (s.channels[0].sampler.pitch == 12.0f && ! s.channels[0].sampler.keepPitch);
    CHECK (clipops::clipSpeed (s.channels[0]) == 2.0);
    CHECK (clipops::fileTicks (s.channels[0], frames, rate, s.bpm) == (std::int64_t) ppq * 2);
    CHECK (s.playlist[0].length == (std::int64_t) ppq * 2 && s.playlist[0].offset == (std::int64_t) ppq / 2);

    // back to 0 with keep length, then +7 st 25 cents keeping length: the clips stay as they are
    clipops::setChannelPitch (s, 0, 0.0, 0.0, true, frames, rate);
    CHECK (s.playlist[0].length == (std::int64_t) ppq * 4 && s.playlist[0].offset == (std::int64_t) ppq);
    clipops::setChannelPitch (s, 0, 7.0, 25.0, true, frames, rate);
    CHECK (std::abs (s.channels[0].sampler.pitch - 7.25f) < 1.0e-6f && s.channels[0].sampler.keepPitch);
    CHECK (clipops::clipSpeed (s.channels[0]) == 1.0);
    CHECK (s.playlist[0].length == (std::int64_t) ppq * 4);

    // a sample channel: only its pitch (its notes have no clips), clamped to ±24
    ChannelDef sampler;
    sampler.instrument = InstrumentKind::sampler;
    s.channels.push_back (sampler);
    clipops::setChannelPitch (s, 1, -30.0, 0.0, false);
    CHECK (s.channels[1].sampler.pitch == -24.0f);
}

TEST (w163c_clip_properties_keep_length_without_a_stretch)
{
    using namespace lynx;
    Song s;
    ChannelDef c;
    c.instrument = InstrumentKind::audioClip;
    s.channels.push_back (c);
    Clip clip;
    clip.kind = ClipKind::audio;
    clip.channel = 0;
    clip.length = ppq;
    s.playlist.push_back (clip);
    clipprops::Props p;
    CHECK (clipprops::read (s, 0, p));
    p.pitch = 3.0f;
    p.keepPitch = true; // used to be dropped unless the clip was stretched
    CHECK (clipprops::apply (s, 0, p));
    CHECK (s.channels[0].sampler.pitch == 3.0f && s.channels[0].sampler.keepPitch);
}
