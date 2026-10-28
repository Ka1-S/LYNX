// REA round 165f-mixer: FL's per-track latency (manual ms, Bypass track latency compensation, Compensate for master
// latency), routing selected ⇄ this track, Create submix, render armed / selected tracks (docs/analise/REA_rodada165f-mixer.md)

namespace latency165
{
// channel c → insert c + 1, all to the Master; insert 1 holds a plugin effect (LatentEffect of `fxLatency`)
inline lynx::Song impulses (int channels, bool latentFx)
{
    lynx::Song r;
    r.mixer = lynx::Song::defaultMixer (channels);
    for (int i = 0; i < channels; ++i)
    {
        lynx::ChannelDef c;
        c.instrument = lynx::InstrumentKind::sampler;
        c.samplePath = "impulse";
        c.volume = 1.0f;
        c.mixerTrack = i + 1;
        r.channels.push_back (c);
    }
    r.patterns.emplace_back();
    if (latentFx)
    {
        lynx::EffectSlot fx;
        fx.kind = lynx::EffectKind::plugin;
        fx.plugin.id = "latent";
        r.mixer[1].effects.push_back (fx);
    }
    return r;
}

inline lynx::RackState::Factories factories (int fxLatency)
{
    lynx::RackState::Factories f;
    f.samples = [] (const std::string&) {
        auto d = std::make_shared<lynx::SampleData>();
        d->sampleRate = 48000.0;
        d->left.assign (64, 0.0f);
        d->left[0] = 1.0f;
        return std::shared_ptr<const lynx::SampleData> (d);
    };
    f.pluginEffects = [fxLatency] (const lynx::PluginRef&, double) -> std::shared_ptr<lynx::Effect> { return std::make_shared<LatentEffect> (fxLatency); };
    return f;
}

// plays a note on `channel` and returns the sample index of each impulse onset at the Master
inline std::vector<int> arrivals (lynx::RackRenderer& renderer, const lynx::RackState& state, bool playing, int channel, int blocks = 2)
{
    lynx::LiveEventQueue live;
    lynx::NoteEvent on;
    on.channel = channel;
    on.key = 60;
    on.velocity = 1.0f;
    live.push (on);
    std::vector<float> l (256), r (256);
    std::vector<int> hits; // the onsets (the sampler's interpolation spreads an impulse over a few samples)
    bool before = false;
    for (int b = 0; b < blocks; ++b)
    {
        renderer.process (&state, playing, 0.0, 256, l.data(), r.data(), b == 0 ? &live : nullptr);
        for (int i = 0; i < 256; ++i)
        {
            const auto hit = std::abs (l[(std::size_t) i]) > 0.01f;
            if (hit && ! before)
                hits.push_back (b * 256 + i);
            before = hit;
        }
    }
    return hits;
}
} // namespace latency165

TEST (latency165_manual_track_latency_in_the_plan)
{
    using namespace lynx;
    // 48 kHz: 100 samples = 2.08333 ms
    Song s = latency165::impulses (3, false);
    const auto ms100 = 100.0f * 1000.0f / 48000.0f;
    CHECK (manualLatencySamples (s.mixer[1], 48000.0) == 0);

    // positive: the track "has" that latency, so the others wait for it (it plays earlier)
    s.mixer[1].latencyMs = ms100;
    auto plan = computePdc (s, { 0, 0, 0 }, {}, 48000.0);
    CHECK (manualLatencySamples (s.mixer[1], 48000.0) == 100);
    CHECK (plan.outLat[1] == 100 && plan.total == 100);
    CHECK (plan.sendDelay[1][0] == 0 && plan.sendDelay[2][0] == 100 && plan.sendDelay[3][0] == 100);

    // negative: this track is delayed (FL's twin swag: −20 ms on insert 18)
    s.mixer[1].latencyMs = -ms100 / 2.0f;
    plan = computePdc (s, { 0, 0, 0 }, {}, 48000.0);
    CHECK (plan.outLat[1] == -50 && plan.total == 0);
    CHECK (plan.sendDelay[1][0] == 50 && plan.sendDelay[2][0] == 0);

    // on top of an effect's latency, through a send (insert 1 → insert 2 → Master)
    s.mixer[1].latencyMs = ms100;
    s.mixer[1].effects.push_back (makeEffectSlot (EffectKind::eq));
    s.mixer[1].toMaster = false;
    s.setRoute (1, 2, true);
    plan = computePdc (s, { 0, 0, 0 }, { {}, { 30 } }, 48000.0);
    CHECK (plan.outLat[1] == 130 && plan.inAlign[2] == 130 && plan.channelDelay[1] == 130);
    CHECK (plan.sendDelay[1][1] == 0 && plan.total == 130 && plan.sendDelay[3][0] == 130);

    // on the Master: only the total grows
    Song m = latency165::impulses (2, false);
    m.mixer[0].latencyMs = ms100;
    plan = computePdc (m, { 0, 0 }, {}, 48000.0);
    CHECK (plan.total == 100 && plan.sendDelay[1][0] == 0 && plan.sendDelay[2][0] == 0);

    // FL's clamp: ±5000 ms
    m.mixer[0].latencyMs = 90000.0f;
    CHECK (manualLatencySamples (m.mixer[0], 1000.0) == 5000);
}

TEST (latency165_defaults_change_nothing)
{
    using namespace lynx;
    // the same plan as before the fields existed: a song with latent effects, default latency fields
    Song s = latency165::impulses (4, false);
    s.mixer[1].effects.push_back (makeEffectSlot (EffectKind::eq));
    s.setRoute (1, 3, true);
    s.setRoute (1, 0, false);
    const auto a = computePdc (s, { 0, 0, 30, 0 }, { {}, { 100 } });
    CHECK (a.inAlign[3] == 100 && a.channelDelay[2] == 70 && a.sendDelay[2][0] == 100 && a.total == 100);
    for (auto& t : s.mixer)
    {
        CHECK (t.latencyMs == 0.0f && t.latencyBypass == LatencyBypass::never && t.compensateMasterLatency);
    }
    // "When recording or not playing" is planned like "Never" (the renderer skips the lines when it applies)
    s.mixer[2].latencyBypass = LatencyBypass::whenRecordingOrStopped;
    const auto b = computePdc (s, { 0, 0, 30, 0 }, { {}, { 100 } });
    CHECK (a.sendDelay == b.sendDelay && a.channelDelay == b.channelDelay && a.total == b.total);
}

TEST (latency165_bypass_always_in_the_plan)
{
    using namespace lynx;
    // insert 1 has a 100-sample plugin; insert 2 is the "mic" track
    Song s = latency165::impulses (2, false);
    s.mixer[1].effects.push_back (makeEffectSlot (EffectKind::eq));
    auto plan = computePdc (s, { 0, 0 }, { {}, { 100 } }, 48000.0);
    CHECK (plan.sendDelay[2][0] == 100);
    s.mixer[2].latencyBypass = LatencyBypass::always;
    plan = computePdc (s, { 0, 0 }, { {}, { 100 } }, 48000.0);
    CHECK (plan.sendDelay[2][0] == 0 && plan.total == 100); // not delayed; the total stays
    // a bypassed slow track still makes the others wait (FL: its outLat counts at the destination)
    s.mixer[2].latencyBypass = LatencyBypass::never;
    s.mixer[1].latencyBypass = LatencyBypass::always;
    plan = computePdc (s, { 0, 0 }, { {}, { 100 } }, 48000.0);
    CHECK (plan.sendDelay[1][0] == 0 && plan.sendDelay[2][0] == 100 && plan.total == 100);
    // sends too
    s.mixer[1].latencyBypass = LatencyBypass::never;
    s.mixer[2].latencyBypass = LatencyBypass::always;
    Song t = latency165::impulses (3, false);
    t.mixer[3].latencyBypass = LatencyBypass::always;
    t.mixer[3].toMaster = false;
    t.setRoute (3, 2, true);
    t.mixer[3].effects.push_back (makeEffectSlot (EffectKind::eq));
    plan = computePdc (t, { 0, 0, 0 }, { {}, {}, {}, { 40 } }, 48000.0);
    CHECK (plan.inAlign[2] == 40 && plan.sendDelay[3][1] == 0);      // 3 → 2 bypassed: not delayed
    CHECK (plan.channelDelay[1] == 40 && plan.channelDelay[2] == 0);  // insert 2's own channel still lines up inside it
}

TEST (latency165_renderer_bypass_when_recording_or_stopped)
{
    using namespace lynx;
    auto song = latency165::impulses (2, true);
    const auto f = latency165::factories (100);
    auto state = RackState::build (song, nullptr, 48000.0, f);
    CHECK (state->pdc.total == 100 && state->sendDelays[2][0] != nullptr);

    {
        // Never: the impulse of insert 2 waits 100 samples, playing or not
        RackRenderer renderer;
        renderer.prepare (48000.0, 256);
        CHECK (latency165::arrivals (renderer, *state, false, 1) == std::vector<int> { 100 });
    }

    song.mixer[2].latencyBypass = LatencyBypass::whenRecordingOrStopped;
    auto idle = RackState::build (song, state.get(), 48000.0, f);
    CHECK (idle->anyBypassWhenIdle && idle->bypassWhenIdle[2] != 0 && idle->sendDelays[2][0] != nullptr);
    {
        RackRenderer renderer;
        renderer.prepare (48000.0, 256);
        // stopped: no compensation (low-latency monitoring)
        CHECK (latency165::arrivals (renderer, *idle, false, 1) == std::vector<int> { 0 });
        // playing: compensated again, and the line starts empty (nothing of the stopped part comes back)
        CHECK (latency165::arrivals (renderer, *idle, true, 1) == std::vector<int> { 100 });
        // recording while playing: bypassed
        LiveInput input;
        input.recording = true;
        renderer.setLiveInput (&input);
        CHECK (latency165::arrivals (renderer, *idle, true, 1) == std::vector<int> { 0 });
        renderer.setLiveInput (nullptr);
        // the latent track itself (insert 1) is not affected
        CHECK (latency165::arrivals (renderer, *idle, false, 0) == std::vector<int> { 100 });
    }

    song.mixer[2].latencyBypass = LatencyBypass::always;
    auto always = RackState::build (song, idle.get(), 48000.0, f);
    CHECK (always->sendDelays[2][0] == nullptr && ! always->anyBypassWhenIdle);
    {
        RackRenderer renderer;
        renderer.prepare (48000.0, 256);
        CHECK (latency165::arrivals (renderer, *always, true, 1) == std::vector<int> { 0 });
    }

    // manual latency rendered: −2.0833 ms on insert 2 (no plugin): its impulse comes 100 samples late
    auto manual = latency165::impulses (2, false);
    manual.mixer[2].latencyMs = -100.0f * 1000.0f / 48000.0f;
    auto late = RackState::build (manual, nullptr, 48000.0, f);
    {
        RackRenderer renderer;
        renderer.prepare (48000.0, 256);
        CHECK (latency165::arrivals (renderer, *late, true, 1) == std::vector<int> { 100 });
        CHECK (latency165::arrivals (renderer, *late, true, 0) == std::vector<int> { 0 });
    }
}

TEST (latency165_hardware_output_compensation)
{
    using namespace lynx;
    auto song = latency165::impulses (2, true);
    song.mixer[2].outputChannel = 2;
    const auto f = latency165::factories (100);
    auto state = RackState::build (song, nullptr, 48000.0, f);
    CHECK (state->hwRoutes.size() == 1 && state->hwRoutes[0].delay != nullptr && state->hwRoutes[0].delay->length() == 100);
    song.mixer[2].compensateMasterLatency = false; // FL's "Compensate for master latency" off
    auto off = RackState::build (song, state.get(), 48000.0, f);
    CHECK (off->hwRoutes.size() == 1 && off->hwRoutes[0].delay == nullptr);
    song.mixer[2].compensateMasterLatency = true;
    song.mixer[2].latencyBypass = LatencyBypass::always;
    auto bypassed = RackState::build (song, off.get(), 48000.0, f);
    CHECK (bypassed->hwRoutes.size() == 1 && bypassed->hwRoutes[0].delay == nullptr);
}

TEST (latency165_delay_line_clear)
{
    lynx::DelayLine line (4);
    float l[4] = { 1, 2, 3, 4 }, r[4] = { 1, 2, 3, 4 };
    line.process (l, r, 4);
    line.clear();
    float out[4] = { 9, 9, 9, 9 }, outR[4] = { 9, 9, 9, 9 };
    line.process (out, outR, 4);
    CHECK (out[0] == 0.0f && out[3] == 0.0f && outR[2] == 0.0f);
}

TEST (latency165_flp_import)
{
    using namespace lynx;
    // FL's insert writer (FUN_00f3d620): 0xEC = f32 manual latency, u32 flags (0x4000 = no master compensation),
    // f32 input latency; 0x31 = the bypass mode, after 0xA5 / 0xA6
    FlpBuilder b;
    auto insert = [&b] (float ms, std::uint32_t flags, int bypass) {
        std::vector<std::uint8_t> params (12, 0);
        std::memcpy (params.data(), &ms, 4);
        std::memcpy (params.data() + 4, &flags, 4);
        b.data (0xEC, params);
        b.u8 (0xA5);
        b.u32 (3);
        b.u8 (0x31);
        b.u8 ((std::uint8_t) bypass);
    };
    insert (0.0f, 0x4C, 0);       // Master
    insert (-20.0f, 0x4C, 0);     // insert 1: twin swag's −20 ms
    insert (12.5f, 0x4C | 0x4000, 1);
    insert (0.0f, 0x4C, 2);
    const auto p = flp::parseProject (b.bytes);
    CHECK (p.mixer.size() == 4 && p.mixer[1].latencyMs == -20.0f && p.mixer[2].latencyMs == 12.5f);
    CHECK (p.mixer.size() == 4 && p.mixer[2].latencyBypass == 1 && p.mixer[3].latencyBypass == 2 && p.mixer[1].recordSource == 3);
    const auto song = importFlp (p);
    CHECK (song.mixer.size() > 3 && song.mixer[1].latencyMs == -20.0f && song.mixer[1].latencyBypass == LatencyBypass::never);
    CHECK (song.mixer.size() > 3 && song.mixer[2].latencyMs == 12.5f && song.mixer[2].latencyBypass == LatencyBypass::always
           && ! song.mixer[2].compensateMasterLatency && song.mixer[1].compensateMasterLatency);
    CHECK (song.mixer.size() > 3 && song.mixer[3].latencyBypass == LatencyBypass::whenRecordingOrStopped);

    // FL's demo "SeamlessR - Bass Antics Performance": insert 7 = −3.49 ms
    if (! realFlpDir.empty())
    {
        const auto path = std::filesystem::path (realFlpDir) / "Demo projects" / "Performance mode" / "SeamlessR - Bass Antics Performance.flp";
        if (std::filesystem::exists (path))
        {
            std::ifstream in (path, std::ios::binary);
            const std::vector<std::uint8_t> file ((std::istreambuf_iterator<char> (in)), {});
            const auto demo = importFlp (flp::parseProject (file));
            CHECK (demo.mixer.size() > 7 && std::abs (demo.mixer[7].latencyMs + 3.492f) < 1e-3f);
            int others = 0;
            for (std::size_t t = 0; t < demo.mixer.size(); ++t)
                others += t != 7 && demo.mixer[t].latencyMs != 0.0f;
            CHECK (others == 0);
        }
    }
}

TEST (latency165_reset_manual_latency)
{
    using namespace lynx;
    Song s = latency165::impulses (3, false);
    s.mixer[1].latencyMs = 5.0f;
    s.mixer[0].latencyMs = -1.0f;
    s.mixer[2].latencyBypass = LatencyBypass::always;
    CHECK (mixerops::resetManualLatency (s) == 2);
    CHECK (s.mixer[0].latencyMs == 0.0f && s.mixer[1].latencyMs == 0.0f && s.mixer[2].latencyBypass == LatencyBypass::always);
    CHECK (mixerops::resetManualLatency (s) == 0);
}

TEST (mixer165_route_selected_and_this_track)
{
    using namespace lynx;
    using mixerops::RouteKind;
    Song s;
    s.mixer = Song::defaultMixer (6);
    // selected 1, 2 → this 4
    CHECK (mixerops::routeSelectedTo (s, { 1, 2, 4 }, 4, RouteKind::route) == 2);
    CHECK (s.routes (1, 4) && s.routes (2, 4) && s.mixer[1].toMaster && std::abs (s.routeLevel (1, 4) - 1.0f) < 1e-6f);
    CHECK (mixerops::routeSelectedTo (s, { 1 }, 5, RouteKind::routeOnly) == 1);
    CHECK (s.routes (1, 5) && ! s.routes (1, 4) && ! s.mixer[1].toMaster);
    // sidechain: a level-0 send and the compressors of the destination listen
    s.mixer[3].effects.push_back (makeEffectSlot (EffectKind::compressor));
    CHECK (mixerops::routeSelectedTo (s, { 2 }, 3, RouteKind::sidechain) == 1);
    CHECK (s.routes (2, 3) && s.routeLevel (2, 3) == 0.0f && s.mixer[3].effects[0].sidechain == 2 && s.routes (2, 4) && s.mixer[2].toMaster);
    CHECK (mixerops::routeSelectedTo (s, { 2 }, 3, RouteKind::sidechainOnly) == 1);
    CHECK (s.routes (2, 3) && ! s.routes (2, 4) && ! s.mixer[2].toMaster);
    CHECK (mixerops::routeSelectedTo (s, { 2 }, 0, RouteKind::sidechain) == 0); // the Master has no side input
    // a cycle is skipped: 5 → 1 while 1 → 5
    CHECK (mixerops::routeSelectedTo (s, { 5 }, 1, RouteKind::route) == 0 && ! s.routes (5, 1));

    // this 6 → selected 3, 4, 6
    Song t;
    t.mixer = Song::defaultMixer (6);
    CHECK (mixerops::routeToSelected (t, 6, { 3, 4, 6 }, RouteKind::route) == 2);
    CHECK (t.routes (6, 3) && t.routes (6, 4) && t.mixer[6].toMaster);
    CHECK (mixerops::routeToSelected (t, 6, { 3 }, RouteKind::routeOnly) == 1);
    CHECK (t.routes (6, 3) && ! t.routes (6, 4) && ! t.mixer[6].toMaster);
    t.mixer[2].effects.push_back (makeEffectSlot (EffectKind::compressor));
    CHECK (mixerops::routeToSelected (t, 6, { 2 }, RouteKind::sidechainOnly) == 1);
    CHECK (t.routes (6, 2) && t.routeLevel (6, 2) == 0.0f && ! t.routes (6, 3) && t.mixer[2].effects[0].sidechain == 6);
    // nothing routable: the track keeps an output
    Song u;
    u.mixer = Song::defaultMixer (3);
    u.setRoute (1, 2, true);
    CHECK (mixerops::routeToSelected (u, 2, { 1 }, RouteKind::routeOnly) == 0 && u.mixer[2].toMaster);
}

TEST (mixer165_create_submix)
{
    using namespace lynx;
    Song s;
    s.mixer = Song::defaultMixer (8);
    s.mixer[6].colour = 0xFF336699;
    s.mixer[3].name = "Snare";
    // selected 2, 3, 4 (contiguous) → this 5 (next to them): "Drums"
    CHECK (mixerops::createSubmix (s, { 2, 3, 4, 5 }, 5, "Drums") == 3);
    for (int t : { 2, 3, 4 })
    {
        CHECK (s.routes (t, 5) && ! s.mixer[(std::size_t) t].toMaster && s.mixer[(std::size_t) t].sends.size() == 1);
    }
    CHECK (s.mixer[5].name == "Drums master" && s.mixer[2].name == "Drums" && s.mixer[3].name == "Snare" && s.mixer[4].name == "Drums");
    CHECK (s.mixer[2].separator && ! s.mixer[3].separator && ! s.mixer[5].separator && s.mixer[6].separator); // 2..5 a group
    CHECK (s.mixer[5].toMaster);

    // not contiguous: no group; the colour follows; no name = names stay
    Song t;
    t.mixer = Song::defaultMixer (8);
    t.mixer[7].colour = 0xFF112233;
    CHECK (mixerops::createSubmix (t, { 1, 3 }, 7, "") == 2);
    CHECK (t.routes (1, 7) && t.routes (3, 7) && t.mixer[1].colour == 0xFF112233 && t.mixer[3].colour == 0xFF112233);
    CHECK (mixerops::hasDefaultName (t, 1) && mixerops::hasDefaultName (t, 7));
    for (std::size_t i = 0; i < t.mixer.size(); ++i)
    {
        CHECK (! t.mixer[i].separator);
    }
    CHECK (mixerops::createSubmix (t, { 1 }, 0, "x") == 0); // the Master can't be a submix
}

TEST (export165_stem_tracks_filter)
{
    using namespace lynx;
    Song s = latency165::impulses (3, false); // channels on inserts 1..3
    s.mixer.push_back (MixerTrack {});          // insert 4: no sound
    CHECK (exportopts::stemTracks (s) == std::vector<int> ({ 1, 2, 3 }));
    CHECK (exportopts::stemTracks (s, {}) == std::vector<int> ({ 1, 2, 3 }));
    CHECK (exportopts::stemTracks (s, { 4, 2, 0, 2, 99 }) == std::vector<int> ({ 2, 4 })); // armed / selected, in order
}
