// Round 165i (perf165): the keep-pitch samplers' pitch shifters are made in parallel when a rack is built

namespace perf165
{
void setEnv (const char* name, const char* value)
{
   #if defined(_WIN32)
    _putenv_s (name, value);
   #else
    ::setenv (name, value, 1);
   #endif
}

std::vector<float> renderTransposed (int channels)
{
    lynx::Song s;
    s.mixer = lynx::Song::defaultMixer (4);
    s.bpm = 120.0;
    s.patterns.emplace_back();
    const auto sample = s162::sine (330.0, 1.0);
    for (int i = 0; i < channels; ++i)
    {
        lynx::ChannelDef c;
        c.instrument = lynx::InstrumentKind::sampler;
        c.samplePath = "sine.wav";
        c.sampler.stretchBeats = 2.0f + (float) (i % 3);
        c.sampler.keepPitch = true;
        c.sampler.stretchMethod = i % 2 == 0 ? lynx::StretchMethod::transients : lynx::StretchMethod::general;
        c.mixerTrack = 1 + i % 3;
        c.volume = 0.3f;
        s.channels.push_back (c);
        for (int n = 0; n < 4; ++n) // transposed notes: they go through the shifters
            s.patterns[0].notes.push_back ({ (std::int64_t) (n * 4) * lynx::ticksPerStep, 3 * lynx::ticksPerStep, i, 55 + 3 * n + i, 0.7f });
    }
    s.patterns[0].sortNotes();
    lynx::RackState::Factories f;
    f.samples = [sample] (const std::string&) { return std::shared_ptr<const lynx::SampleData> (sample); };
    f.stretchedSamples = [sample] (const std::string&, double ratio, lynx::StretchMethod m) {
        return std::shared_ptr<const lynx::SampleData> (lynx::timeStretch (*sample, ratio, m));
    };
    auto state = lynx::RackState::build (s, nullptr, 44100.0, f);
    lynx::RackRenderer r;
    r.prepare (44100.0, 512);
    std::vector<float> out, l (441), rr (441);
    const auto tps = lynx::ticksPerSample (s.bpm, 44100.0);
    for (int block = 0; block < 200; ++block)
    {
        r.process (state.get(), true, block * 441 * tps, 441, l.data(), rr.data());
        out.insert (out.end(), l.begin(), l.end());
        out.insert (out.end(), rr.begin(), rr.end());
    }
    return out;
}
} // namespace perf165

TEST (perf165_parallel_key_shifters_render_the_same)
{
    perf165::setEnv ("LYNX_PARALLEL_KEYSHIFT", "0");
    const auto serial = perf165::renderTransposed (6);
    const auto again = perf165::renderTransposed (6);
    CHECK (serial == again); // (the render itself repeats)
    perf165::setEnv ("LYNX_PARALLEL_KEYSHIFT", "1");
    const auto parallel = perf165::renderTransposed (6);
    CHECK (serial.size() == parallel.size());
    CHECK (serial == parallel);
    CHECK (*std::max_element (parallel.begin(), parallel.end()) > 0.02f);
    const lynx::ScopedFlushDenormals ftz; // also when the building thread flushes denormals (the helpers copy its mode)
    perf165::setEnv ("LYNX_PARALLEL_KEYSHIFT", "0");
    const auto serialFtz = perf165::renderTransposed (6);
    perf165::setEnv ("LYNX_PARALLEL_KEYSHIFT", "1");
    CHECK (serialFtz == perf165::renderTransposed (6));
}

TEST (perf165_long_stretches_repeat_exactly)
{
    // beyond 2× Signalsmith randomises the bins' time steps: with a random seed every open/export differed
    const auto sample = s162::sine (330.0, 0.5);
    for (const auto method : { lynx::StretchMethod::general, lynx::StretchMethod::transients, lynx::StretchMethod::mono })
    {
        const auto a = lynx::timeStretch (*sample, 3.1, method);
        const auto b = lynx::timeStretch (*sample, 3.1, method);
        CHECK (a != nullptr && b != nullptr);
        if (a != nullptr && b != nullptr)
        {
            CHECK (a->left == b->left);
            CHECK (a->right == b->right);
            CHECK (std::abs ((double) a->frames() - 3.1 * (double) sample->frames()) < 2.0);
        }
    }
}
