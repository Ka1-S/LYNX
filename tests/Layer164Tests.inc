// Round 164a: FL's Layer channel (core/Layer.h) and the channels' key region

namespace layer164
{
// a hosted "plugin" that records the notes it gets
struct NoteSpy final : lynx::Instrument
{
    struct Got
    {
        bool on = true;
        int key = 0;
        float velocity = 0.0f;
    };
    std::vector<Got> got;
    void prepare (double) noexcept override {}
    void noteOn (int key, float velocity) noexcept override { got.push_back ({ true, key, velocity }); }
    void noteOff (int key) noexcept override { got.push_back ({ false, key, 0.0f }); }
    void allNotesOff() noexcept override {}
    void render (float*, float*, int, float, float) noexcept override {}
    int ons() const
    {
        int n = 0;
        for (const auto& g : got)
            n += g.on ? 1 : 0;
        return n;
    }
};

struct Rig
{
    lynx::Song song;
    std::vector<std::shared_ptr<NoteSpy>> spies; // [channel] (the Layer's own is unused)
    std::unique_ptr<const lynx::RackState> state;
    lynx::RackRenderer renderer;
    lynx::LiveEventQueue live;

    // channel 0 = the Layer, channels 1…children = spies
    explicit Rig (int children, std::function<void (lynx::Song&)> setup = {})
    {
        song.mixer = lynx::Song::defaultMixer (2);
        lynx::ChannelDef l;
        l.name = "Layer";
        l.instrument = lynx::InstrumentKind::layer;
        song.channels.push_back (l);
        for (int k = 0; k < children; ++k)
        {
            lynx::ChannelDef c;
            c.instrument = lynx::InstrumentKind::plugin;
            c.plugin.id = "spy" + std::to_string (k + 1);
            song.channels.push_back (c);
            song.channels[0].layer.children.push_back (k + 1);
        }
        song.patterns.emplace_back();
        if (setup)
            setup (song);
        spies.resize (song.channels.size());
        lynx::RackState::Factories f;
        f.pluginInstruments = [this] (const lynx::PluginRef& ref, double) {
            const auto k = std::stoi (ref.id.substr (3));
            spies[(std::size_t) k] = std::make_shared<NoteSpy>();
            return std::shared_ptr<lynx::Instrument> (spies[(std::size_t) k]);
        };
        state = lynx::RackState::build (song, nullptr, 44100.0, f);
        renderer.prepare (44100.0, 256);
    }

    void play (int channel, int key, bool on = true)
    {
        lynx::NoteEvent e;
        e.type = on ? lynx::NoteEvent::Type::on : lynx::NoteEvent::Type::off;
        e.channel = channel;
        e.key = key;
        e.velocity = on ? 0.8f : 0.0f;
        live.push (e);
        std::vector<float> l (256), r (256);
        renderer.process (state.get(), false, 0.0, 256, l.data(), r.data(), &live, nullptr);
    }
};
} // namespace layer164

TEST (layer164_every_child_plays_the_layers_notes)
{
    layer164::Rig rig (3);
    rig.play (0, 60);
    rig.play (0, 60, false);
    for (int k = 1; k <= 3; ++k)
    {
        CHECK (rig.spies[(std::size_t) k]->ons() == 1);
        CHECK (rig.spies[(std::size_t) k]->got.size() == 2);
        CHECK (rig.spies[(std::size_t) k]->got[0].key == 60);
        CHECK (! rig.spies[(std::size_t) k]->got[1].on);
    }
    // a child's own notes don't go to its siblings
    rig.play (2, 64);
    CHECK (rig.spies[2]->ons() == 2);
    CHECK (rig.spies[1]->ons() == 1 && rig.spies[3]->ons() == 1);
}

TEST (layer164_random_plays_one_child_and_its_off_reaches_it)
{
    layer164::Rig rig (4, [] (lynx::Song& s) { s.channels[0].layer.random = true; });
    std::array<int, 5> count {};
    for (int n = 0; n < 200; ++n)
    {
        rig.play (0, 60);
        rig.play (0, 60, false);
    }
    int total = 0;
    for (int k = 1; k <= 4; ++k)
    {
        count[(std::size_t) k] = rig.spies[(std::size_t) k]->ons();
        total += count[(std::size_t) k];
        CHECK (count[(std::size_t) k] > 20); // every child gets picked
        // each child got as many offs as ons
        CHECK ((int) rig.spies[(std::size_t) k]->got.size() == 2 * count[(std::size_t) k]);
    }
    CHECK (total == 200);
}

TEST (layer164_sequential_plays_the_children_in_turn)
{
    layer164::Rig rig (3, [] (lynx::Song& s) { s.channels[0].layer.sequential = true; });
    std::vector<int> order;
    for (int n = 0; n < 6; ++n)
    {
        rig.play (0, 60);
        for (int k = 1; k <= 3; ++k)
            if (rig.spies[(std::size_t) k]->ons() > (int) std::count (order.begin(), order.end(), k))
                order.push_back (k);
        rig.play (0, 60, false);
    }
    CHECK (order == std::vector<int> ({ 1, 2, 3, 1, 2, 3 }));
}

TEST (layer164_crossfade_gains)
{
    using lynx::layer::crossfadeGain;
    // three children: child 1 at 0, 1+2 at 0.25, child 2 at 0.5, 2+3 at 0.75, child 3 at 1
    CHECK (crossfadeGain (0, 3, 0.0f) == 1.0f && crossfadeGain (1, 3, 0.0f) == 0.0f);
    CHECK (std::abs (crossfadeGain (1, 3, 0.5f) - 1.0f) < 1e-6f && crossfadeGain (0, 3, 0.5f) == 0.0f && crossfadeGain (2, 3, 0.5f) == 0.0f);
    CHECK (std::abs (crossfadeGain (0, 3, 0.25f) - crossfadeGain (1, 3, 0.25f)) < 1e-6f);
    CHECK (std::abs (crossfadeGain (0, 1, 0.7f) - 0.7f) < 1e-6f); // (one child: the Fade itself, as FL)
    CHECK (std::abs (crossfadeGain (0, 3, 0.25f) - 0.5f) < 1e-6f); // FL's linear triangle
    // a crossfaded Layer only plays the children with a level
    layer164::Rig rig (3, [] (lynx::Song& s) { s.channels[0].layer.crossfade = true; s.channels[0].layer.fade = 1.0f; });
    rig.play (0, 60);
    CHECK (rig.spies[1]->ons() == 0 && rig.spies[2]->ons() == 0 && rig.spies[3]->ons() == 1);
    // FL scales the child by the Layer's channel gain (a plugin has no voice gain: the velocity carries it)
    CHECK (std::abs (rig.spies[3]->got[0].velocity - 0.8f * lynx::channelGain (0.78f)) < 1e-5f);
}

TEST (layer164_key_region_and_split_children)
{
    layer164::Rig rig (3, [] (lynx::Song& s) {
        s.channels[0].misc.rootNote = 36;
        lynx::layer::splitChildren (s, 0);
    });
    CHECK (rig.song.channels[1].keyLow == 36 && rig.song.channels[1].keyHigh == 36);
    CHECK (rig.song.channels[3].keyLow == 38 && rig.song.channels[3].keyHigh == 38);
    CHECK (rig.song.channels[2].misc.rootNote == 37 && rig.song.channels[2].misc.addToKey);
    rig.play (0, 37);
    CHECK (rig.spies[1]->ons() == 0 && rig.spies[2]->ons() == 1 && rig.spies[3]->ons() == 0);
    // the key region holds for the channel's own notes too
    rig.play (3, 50);
    CHECK (rig.spies[3]->ons() == 0);
    rig.play (3, 38);
    CHECK (rig.spies[3]->ons() == 1);
    // reset: the whole keyboard, root back on C5
    lynx::layer::resetChildren (rig.song, 0);
    CHECK (rig.song.channels[2].keyLow == 0 && rig.song.channels[2].keyHigh == 127 && rig.song.channels[2].misc.rootNote == 60
           && ! rig.song.channels[2].misc.addToKey);
}

TEST (layer164_split_keeps_a_samplers_pitch_on_its_key)
{
    lynx::Song s;
    lynx::ChannelDef l, smp;
    l.instrument = lynx::InstrumentKind::layer;
    l.misc.rootNote = 48;
    smp.instrument = lynx::InstrumentKind::sampler;
    s.channels = { l, smp, smp };
    lynx::layer::setChildren (s, 0, { 2, 1, 0, 1, 7 });
    CHECK (s.channels[0].layer.children == std::vector<int> ({ 1, 2 }));
    lynx::layer::splitChildren (s, 0);
    // key 49 plays the second sampler at its own pitch: root 49 → pitch +11 semitones over the C5 default
    CHECK (s.channels[2].misc.rootNote == 49 && std::abs (s.channels[2].sampler.pitch - 11.0f) < 1e-6f);
    lynx::layer::resetChildren (s, 0);
    CHECK (s.channels[2].misc.rootNote == 60 && std::abs (s.channels[2].sampler.pitch) < 1e-6f);
}

TEST (layer164_children_follow_channel_edits)
{
    lynx::Song s;
    lynx::ChannelDef l, c;
    l.instrument = lynx::InstrumentKind::layer;
    s.channels = { c, l, c, c, c };
    s.channels[1].layer.children = { 0, 2, 4 };
    s.removeChannel (2);
    CHECK (s.channels[1].layer.children == std::vector<int> ({ 0, 3 }));
    CHECK (lynx::layer::parentsOf (s, 3) == std::vector<int> { 1 });
    lynx::permuteChannels (s, { 3, 0, 1, 2 }); // channel 3 first
    CHECK (s.channels[2].instrument == lynx::InstrumentKind::layer);
    CHECK (s.channels[2].layer.children == std::vector<int> ({ 1, 0 }));
}

// ===== round 164: X-Y / X-Y-Z Controller, Control Surface, Dashboard =====

TEST (xy164_shape_and_output)
{
    // straight at 0.5, FL's tension bends it either way and keeps the ends
    CHECK (std::abs (lynx::xy::shape (0.3, 0.5f) - 0.3) < 1e-9);
    for (const auto t : { 0.0f, 0.25f, 0.75f, 1.0f })
        CHECK (std::abs (lynx::xy::shape (0.0, t)) < 1e-9 && std::abs (lynx::xy::shape (1.0, t) - 1.0) < 1e-9);
    CHECK (lynx::xy::shape (0.5, 1.0f) > 0.5 && lynx::xy::shape (0.5, 0.0f) < 0.5);
    auto p = lynx::xy::defaults (lynx::EffectKind::xyController);
    p[(std::size_t) lynx::xy::minimum (1)] = 0.2f;
    p[(std::size_t) lynx::xy::maximum (1)] = 0.6f;
    CHECK (std::abs (lynx::xy::output (lynx::EffectKind::xyController, p, 1, 0.5) - 0.4f) < 1e-6f);
    // min above max inverts the axis
    p[(std::size_t) lynx::xy::minimum (1)] = 1.0f;
    p[(std::size_t) lynx::xy::maximum (1)] = 0.0f;
    CHECK (std::abs (lynx::xy::output (lynx::EffectKind::xyController, p, 1, 0.25) - 0.75f) < 1e-6f);
}

TEST (xy164_effect_follows_goal)
{
    for (const auto kind : { lynx::EffectKind::xyController, lynx::EffectKind::xyzController })
    {
        auto slot = lynx::makeEffectSlot (kind);
        CHECK ((int) lynx::effectInfo (kind).params.size() == lynx::xy::paramCount (kind));
        CHECK (slot.params[(std::size_t) lynx::xy::level (0)] == 0.5f);
        auto fx = lynx::makeEffect (slot);
        fx->prepare (44100.0);
        std::vector<float> l (512, 0.25f), r (512, -0.25f);
        fx->process (l.data(), r.data(), 512, slot.params, 120.0);
        CHECK (l[0] == 0.25f && r[0] == -0.25f); // no change to the sound
        CHECK (std::abs (fx->controlOutput (0) - 0.5f) < 1e-6f && fx->controlOutput (7) == 1.0f);
        // the goal jumps: the target moves there smoothly (never past it) and arrives
        slot.params[(std::size_t) lynx::xy::level (0)] = 1.0f;
        float last = 0.5f;
        int blocks = 0;
        bool monotonic = true;
        while (fx->controlOutput (4) < 1.0f && blocks < 2000)
        {
            fx->process (l.data(), r.data(), 512, slot.params, 120.0);
            monotonic = monotonic && fx->controlOutput (4) >= last && fx->controlOutput (4) <= 1.0f;
            last = fx->controlOutput (4);
            ++blocks;
        }
        CHECK (monotonic && blocks > 2 && blocks < 2000);
        CHECK (std::abs (fx->controlOutput (0) - 1.0f) < 1e-6f);
        CHECK (std::abs (fx->controlOutput (1) - 0.5f) < 1e-6f); // Y untouched
    }
    // faster speed arrives sooner
    auto time = [] (float speed) {
        auto slot = lynx::makeEffectSlot (lynx::EffectKind::xyController);
        slot.params[(std::size_t) lynx::xy::speed (slot.kind)] = speed;
        auto fx = lynx::makeEffect (slot);
        fx->prepare (44100.0);
        std::vector<float> l (256), r (256);
        fx->process (l.data(), r.data(), 256, slot.params, 120.0);
        slot.params[0] = 0.0f;
        int blocks = 0;
        while (fx->controlOutput (4) > 0.0f && blocks < 100000)
            fx->process (l.data(), r.data(), 256, slot.params, 120.0), ++blocks;
        return blocks;
    };
    CHECK (time (0.9f) < time (0.2f));
}

TEST (xy164_fl_state)
{
    // FL's X-Y state: version, then X level/min/max/tension, Y …, speed 1024…2048, accel 256…1024
    std::vector<std::uint8_t> d (44, 0);
    const std::int32_t values[] = { 1, 16384, 0, 65536, 64, 65536, 6553, 58982, 0, 1536, 640 };
    std::memcpy (d.data(), values, sizeof (values));
    const auto slot = lynx::xy::fromFl ("Fruity X-Y Controller", d);
    CHECK (slot.has_value());
    if (! slot)
        return;
    CHECK (slot->kind == lynx::EffectKind::xyController);
    CHECK (std::abs (slot->params[0] - 0.25f) < 1e-4f && std::abs (slot->params[3] - 0.75f) < 1e-4f);
    CHECK (std::abs (slot->params[4] - 1.0f) < 1e-4f && std::abs (slot->params[5] - 0.1f) < 1e-3f);
    CHECK (std::abs (slot->params[8] - 0.5f) < 1e-4f && std::abs (slot->params[9] - 0.5f) < 1e-4f);
    // the output equals the constant value the old import applied (core/FlEffects.h controllerConstantOutput)
    for (int o = 0; o < 2; ++o)
    {
        const auto constant = lynx::controllerConstantOutput ("Fruity X-Y Controller", d, o);
        CHECK (constant && std::abs (*constant - lynx::xy::output (slot->kind, slot->params, o, slot->params[(std::size_t) lynx::xy::level (o)])) < 1e-3f);
    }
}

TEST (surface164_outputs_and_params)
{
    auto slot = lynx::makeEffectSlot (lynx::EffectKind::controlSurface);
    lynx::SurfaceControl knob, pad, button;
    knob.name = "Cutoff";
    knob.value = 0.3f;
    pad.kind = lynx::SurfaceControl::Kind::xy;
    pad.name = "Pad";
    pad.value = 0.1f;
    pad.valueY = 0.9f;
    button.kind = lynx::SurfaceControl::Kind::button;
    button.name = "Go";
    CHECK (lynx::surface::add (slot, knob) == 0 && lynx::surface::add (slot, pad) == 1 && lynx::surface::add (slot, button) == 2);
    CHECK (lynx::surface::firstOutput (slot.patcher.surface, 2) == 3);
    CHECK (lynx::surface::outputName (slot.patcher.surface, 2) == "Pad Y" && lynx::surface::outputName (slot.patcher.surface, 3) == "Go");
    CHECK (slot.params[0] == 0.3f && slot.params[1] == 0.1f && slot.params[2] == 0.9f);
    lynx::surface::setValue (slot, 2, 1.0f, 0.0f);
    CHECK (slot.params[3] == 1.0f);
    lynx::surface::remove (slot, 0); // the others move up
    CHECK (slot.params[0] == 0.1f && slot.params[1] == 0.9f && slot.params[2] == 1.0f);
    auto fx = lynx::makeEffect (slot);
    fx->prepare (44100.0);
    std::vector<float> l (64), r (64);
    fx->process (l.data(), r.data(), 64, slot.params, 120.0);
    CHECK (fx->controlOutput (1) == 0.9f);
    // its Patcher node publishes one output per value
    lynx::PatchNode node;
    node.effect = slot;
    CHECK (lynx::patcher::nodePorts (node).controlOuts == 3);
    // a Dashboard channel: the same from the channel's controls
    lynx::ChannelDef dash;
    dash.instrument = lynx::InstrumentKind::dashboard;
    dash.patcher.surface = slot.patcher.surface;
    auto instrument = lynx::makeInstrument (lynx::InstrumentKind::dashboard, nullptr);
    instrument->configure (dash, 120.0);
    CHECK (instrument->controlOutput (2) == 1.0f && instrument->controlOutput (0) == 0.1f);
}

TEST (surface164_fl_demos)
{
    if (realFlpDir.empty())
        return;
    const auto path = std::filesystem::path (realFlpDir) / "Templates" / "Advanced" / "Electronic" / "Trap.flp";
    if (! std::filesystem::exists (path))
        return;
    std::ifstream in (path, std::ios::binary);
    const std::vector<std::uint8_t> file ((std::istreambuf_iterator<char> (in)), {});
    lynx::FlpImportReport report;
    lynx::FlpImportOptions options;
    options.report = &report;
    const auto song = lynx::importFlp (flp::parseProject (file), options);
    int surfaces = 0, controls = 0, links = 0;
    for (const auto& track : song.mixer)
        for (const auto& fx : track.effects)
            if (fx.kind == lynx::EffectKind::controlSurface)
                ++surfaces, controls += (int) fx.patcher.surface.size();
    for (const auto& a : song.automations)
        if (a.isLive() && a.source.channel < 0 && a.source.track >= 0
            && song.mixer[(std::size_t) a.source.track].effects[(std::size_t) a.source.slot].kind == lynx::EffectKind::controlSurface)
            ++links;
    std::printf ("  Trap template: %d Control Surface(s), %d controls, %d live links\n", surfaces, controls, links);
    CHECK (surfaces >= 1 && controls >= 3 && links >= 1);
}

TEST (layer164_fl_flags_and_pitch)
{
    lynx::LayerSettings l;
    lynx::applyFlLayerFlags (1, l);
    CHECK (l.random && ! l.crossfade && ! l.sequential);
    lynx::applyFlLayerFlags (2 | 4, l);
    CHECK (! l.random && l.crossfade && l.sequential);
    lynx::applyFlLayerFlags (1 | 4, l); // (FL checks Random first)
    CHECK (l.random && ! l.sequential);
    lynx::ChannelDef layer;
    layer.misc.rootNote = 56; // RawFL's layer: its children play 4 semitones up
    CHECK (std::abs (lynx::layer::pitchCents (layer) - 400.0f) < 1e-3f);
    layer.sampler.pitch = -1.0f;
    CHECK (std::abs (lynx::layer::pitchCents (layer) - 300.0f) < 1e-3f);
}

namespace volt164
{
// FL's state of the Voltage Controller in the render test (REA_rodada164a §6): version 2, on, main L = pitch,
// main R = gate × 0.5 (scale 49152), clock 24 PPQ, pulses of 15 samples
const std::vector<std::uint8_t> flState {
    0x02,0x00,0x00,0x00,0x00,0x80,0x00,0x00,0x00,0x80,0x00,0x00,0x00,0x80,0x00,0x00,0x00,0x80,0x00,0x00,0x00,0x80,0x00,0x00,
    0x00,0x80,0x00,0x00,0x00,0x80,0x00,0x00,0x00,0x80,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0xc0,0x00,0x00,0x00,0x00,0x01,0x00,
    0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x00,0x00,0x01,0x03,0x05,
    0x06,0x07,0x09,0x09,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x18,0x00,0x00,0x00,0x0f,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xf0,0x3f };

struct Rig
{
    std::unique_ptr<lynx::Instrument> instrument = lynx::voltage::makeInstrument();
    std::vector<float> left, right;
    explicit Rig (const lynx::voltage::Settings& s, double bpm = 120.0)
    {
        lynx::ChannelDef c;
        c.instrument = lynx::InstrumentKind::voltage;
        c.pluginState = lynx::voltage::write (s);
        instrument->prepare (44100.0);
        instrument->configure (c, bpm);
    }
    void run (int n, double ticks = 0.0)
    {
        left.assign ((std::size_t) n, 0.0f);
        right.assign ((std::size_t) n, 0.0f);
        instrument->setSongPosition (ticks);
        instrument->beginChunk (n);
        instrument->render (left.data(), right.data(), n, 1.0f, 1.0f);
    }
};
} // namespace volt164

TEST (voltage164_fl_state)
{
    const auto s = lynx::voltage::parse (volt164::flState);
    CHECK (s.has_value());
    if (! s)
        return;
    CHECK (s->enabled && s->range == 0 && s->pitchStandard == 0 && s->clockPpq == 24 && s->pulseLength == 15);
    CHECK (s->source[0] == lynx::voltage::Source::pitch && s->source[1] == lynx::voltage::Source::gate);
    CHECK (s->source[2] == lynx::voltage::Source::velocity && s->source[7] == lynx::voltage::Source::none);
    CHECK (s->params[9] == 49152 && s->params[8] == 65536 && s->params[0] == 32768);
    CHECK (std::abs (lynx::voltage::scaleOf (*s, 1) - 0.5f) < 1e-6f && std::abs (lynx::voltage::offsetOf (*s, 0)) < 1e-6f);
    // LYNX writes version 3, which reads back the same
    const auto again = lynx::voltage::parse (lynx::voltage::write (*s));
    CHECK (again.has_value() && *again == *s);
    CHECK (! lynx::voltage::parse ({ 0x07, 0x00 }).has_value());
}

TEST (voltage164_matches_fl_render)
{
    // FL's render of the test project: C0, C1 … C10 one after the other, then C5 — main L steps −0.5 … +0.5 by 0.1
    // (1 V / octave at 10 V full scale) and holds after the note ends; main R is the gate × 0.5
    volt164::Rig rig (*lynx::voltage::parse (volt164::flState));
    for (int octave = 0; octave <= 10; ++octave)
    {
        rig.instrument->noteOn (12 * octave, 0.8f);
        rig.run (64);
        CHECK (std::abs (rig.left[63] - (-0.5f + 0.1f * (float) octave)) < 1e-5f);
        CHECK (std::abs (rig.right[63] - 0.5f) < 1e-6f);
        rig.instrument->noteOff (12 * octave);
        rig.run (64);
        CHECK (std::abs (rig.left[63] - (-0.5f + 0.1f * (float) octave)) < 1e-5f); // (pitch holds)
        CHECK (rig.right[63] == 0.0f);
    }
    rig.instrument->noteOn (60, 0.8f);
    rig.run (16);
    CHECK (std::abs (rig.left[15]) < 1e-6f);
    // the channel's volume does not scale the voltages (FL's test channel was at 78 %); muted = silent
    rig.instrument->noteOn (72, 0.8f);
    std::vector<float> l (16, 0.0f), r (16, 0.0f);
    rig.instrument->beginChunk (16);
    rig.instrument->render (l.data(), r.data(), 16, 0.4f, 0.3f);
    CHECK (std::abs (l[15] - 0.1f) < 1e-5f && std::abs (r[15] - 0.5f) < 1e-6f);
    std::fill (l.begin(), l.end(), 0.0f);
    rig.instrument->beginChunk (16);
    rig.instrument->render (l.data(), r.data(), 16, 0.0f, 0.0f);
    CHECK (l[15] == 0.0f);
}

TEST (voltage164_pulses_ranges_and_outputs)
{
    using namespace lynx::voltage;
    Settings s;
    s.enabled = true;
    s.source = { Source::trigger, Source::clock, Source::velocity, Source::none, Source::none, Source::none, Source::none, Source::none };
    s.pulseLength = 15;
    s.clockPpq = 4;
    s.tracks = { 3, 0, 0 };
    volt164::Rig rig (s);
    // a trigger: one pulse of 15 samples at the note's start
    rig.instrument->noteOn (60, 0.5f);
    rig.run (100);
    int high = 0;
    for (const auto v : rig.left)
        high += v > 0.5f ? 1 : 0;
    CHECK (high == 15 && rig.left[0] > 0.5f && rig.left[15] < 0.5f);
    // velocity on Out 2 L, fed to insert 3 (the rack adds that output there)
    CHECK (rig.instrument->auxOutputCount() == 3 && rig.instrument->auxOutputTrack (0) == 3 && rig.instrument->auxOutputTrack (1) == -1);
    CHECK (std::abs (rig.instrument->auxOutputData (0, 0)[50] - 0.5f) < 1e-6f);
    // the clock: 4 pulses per quarter note while the song plays (120 BPM: 22050 samples per beat → 4 pulses)
    int pulses = 0;
    double ticks = 960.0;
    for (int chunk = 0; chunk < 44; ++chunk) // 44 × 512 ≈ one beat past the first chunk
    {
        rig.run (512, ticks);
        for (int i = 0; i < 512; ++i)
            pulses += rig.right[(std::size_t) i] > 0.5f && (i == 0 ? false : rig.right[(std::size_t) i - 1] <= 0.5f) ? 1 : 0;
        ticks += 512.0 * 960.0 * 120.0 / (60.0 * 44100.0);
    }
    CHECK (pulses >= 3 && pulses <= 5);
    // the ranges (full scale 1.0 = 10 V)
    CHECK (applyRange (0, 1.5f) == 1.0f && applyRange (1, 1.0f) == 0.5f && applyRange (2, -1.0f) == 0.0f && applyRange (2, 1.0f) == 1.0f);
    // off (FL's default for a new one): silence and idle
    volt164::Rig off (Settings {});
    off.instrument->noteOn (72, 1.0f);
    off.run (32);
    CHECK (off.instrument->idle() && off.left[31] == 0.0f && off.right[31] == 0.0f);
}
