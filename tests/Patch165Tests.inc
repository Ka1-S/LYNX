// --- Round 165c patch165: the Hardware Insert inside a Patcher, the Keyboard Controller's editor state

namespace patch165
{
lynx::PatchNode hwNode (int roundTrip, bool invert = false)
{
    lynx::PatchNode n;
    n.effect = lynx::makeEffectSlot (lynx::EffectKind::hardwareInsert);
    n.effect.params = hw164::routed (roundTrip);
    n.effect.params[lynx::hwinsert::invert] = invert ? 1.0f : 0.0f;
    return n;
}

// A Patcher (mixer slot) whose input goes through a Hardware Insert and, in parallel, straight to its output
lynx::EffectSlot parallelSlot (int roundTrip)
{
    auto slot = lynx::makeEffectSlot (lynx::EffectKind::patcher);
    slot.patcher.nodes = { hwNode (roundTrip) };
    slot.patcher.wires = { patcher141::audio (lynx::patchFromHost, 0), patcher141::audio (0, lynx::patchToHost),
                           patcher141::audio (lynx::patchFromHost, lynx::patchToHost) };
    return slot;
}
} // namespace patch165

TEST (patch165_hw_insert_in_a_mixer_slot_patcher_lines_up)
{
    using namespace lynx;
    // through the simulated cable at its round trip, the Patcher reports the round trip as its latency and delays its own
    // dry branch by it: output = 2 × input, `roundTrip` later, to the sample — for blocks split into chunks and blocks
    // longer than the graph's piece (the device offset has to follow both splits)
    std::mt19937 rng (165);
    std::uniform_real_distribution<float> u (-0.5f, 0.5f);
    for (const auto [roundTrip, block, chunk] : { std::tuple { 512, 512, 512 }, std::tuple { 371, 128, 64 }, std::tuple { 64, 64, 17 },
                                                  std::tuple { 5000, 4096, 4096 }, std::tuple { 4800, 4800, 1600 },
                                                  std::tuple { 6000, 4800, 4800 } }) // (the device hands out input it already has: round trip >= block)
    {
        const auto slot = patch165::parallelSlot (roundTrip);
        auto fx = patcher::makeEffect (slot, 48000.0, patcher::builtInFactories (48000.0));
        fx->prepare (48000.0);
        CHECK (fx->latencySamples() == roundTrip);
        hw164::LoopDevice dev;
        dev.roundTrip = roundTrip;
        std::vector<float> in, out;
        for (int b = 0; b < 30; ++b)
        {
            std::vector<float> l ((std::size_t) block), r ((std::size_t) block);
            for (int i = 0; i < block; ++i)
                l[(std::size_t) i] = r[(std::size_t) i] = u (rng);
            in.insert (in.end(), l.begin(), l.end());
            dev.callback (*fx, slot.params, l.data(), r.data(), block, chunk);
            out.insert (out.end(), l.begin(), l.end());
        }
        double worst = 0.0;
        for (std::size_t t = (std::size_t) roundTrip; t < out.size(); ++t)
            worst = std::max (worst, (double) std::abs (out[t] - 2.0f * in[t - (std::size_t) roundTrip]));
        if (worst > 1e-5)
            std::printf ("  round trip %d block %d chunk %d: worst %g\n", roundTrip, block, chunk, worst);
        CHECK (worst <= 1e-5);
    }
}

TEST (patch165_offline_patcher_hw_insert_passes_dry_at_its_latency)
{
    using namespace lynx;
    // an export (no device): the insert passes its dry signal delayed by the round trip, so the parallel branches still
    // line up (output = 2 × input, delayed)
    const auto slot = patch165::parallelSlot (300);
    auto fx = patcher::makeEffect (slot, 48000.0, patcher::builtInFactories (48000.0));
    fx->prepare (48000.0);
    std::vector<float> in, out;
    for (int b = 0; b < 8; ++b)
    {
        std::vector<float> l (256), r (256);
        for (int i = 0; i < 256; ++i)
            l[(std::size_t) i] = r[(std::size_t) i] = std::sin (0.01f * (float) (b * 256 + i));
        in.insert (in.end(), l.begin(), l.end());
        fx->setHardwareIo (nullptr, nullptr, 0);
        fx->process (l.data(), r.data(), 256, slot.params, 120.0);
        out.insert (out.end(), l.begin(), l.end());
    }
    double worst = 0.0;
    for (std::size_t t = 300; t < out.size(); ++t)
        worst = std::max (worst, (double) std::abs (out[t] - 2.0f * in[t - 300]));
    CHECK (worst <= 1e-5);
}

TEST (patch165_generator_patcher_hw_insert_follows_the_chunk)
{
    using namespace lynx;
    // a synth inside a generator Patcher goes through the insert (phase inverted) and straight out: when the device
    // offset follows the chunks the two branches cancel exactly; without the straight branch the synth is heard
    for (const auto withDry : { true, false })
    {
        ChannelDef channel;
        channel.instrument = InstrumentKind::patcher;
        constexpr int roundTrip = 512; // (>= the callback: the simulated cable returns what already went out)
        channel.patcher.nodes = { patcher164f::generator (InstrumentKind::synth), patch165::hwNode (roundTrip, true) };
        channel.patcher.wires = { patcher162::midi (patchFromHost, 0, 0, 0), patcher141::audio (0, 1), patcher141::audio (1, patchToHost) };
        if (withDry)
            channel.patcher.wires.push_back (patcher141::audio (0, patchToHost));
        CHECK (patcher::usesHardwareIo (channel.patcher));
        auto inst = patcher::makeInstrument (channel, 48000.0, patcher::builtInFactories (48000.0));
        inst->prepare (48000.0);
        CHECK (inst->latencySamples() == roundTrip);
        inst->noteOn (57, 0.9f);
        hw164::LoopDevice dev;
        dev.roundTrip = roundTrip;
        double energy = 0.0;
        for (int b = 0; b < 40; ++b)
        {
            constexpr int n = 480, chunk = 160;
            const auto at = (std::int64_t) dev.outA.size();
            dev.inA.assign ((std::size_t) n, 0.0f);
            dev.inB.assign ((std::size_t) n, 0.0f);
            for (int i = 0; i < n; ++i)
            {
                dev.inA[(std::size_t) i] = dev.returned (dev.outA, at + i);
                dev.inB[(std::size_t) i] = dev.returned (dev.outB, at + i);
            }
            dev.input.numChannels = 2;
            dev.input.channels[0] = dev.inA.data();
            dev.input.channels[1] = dev.inB.data();
            dev.bus.active.store (true);
            std::vector<float> l ((std::size_t) n, 0.0f), r ((std::size_t) n, 0.0f);
            for (int done = 0; done < n; done += chunk)
            {
                inst->beginChunk (chunk);
                inst->setHardwareIo (&dev.input, &dev.bus, done);
                // (the rack splits a chunk at its notes: two renders here)
                inst->render (l.data() + done, r.data() + done, 100, 1.0f, 1.0f);
                inst->render (l.data() + done + 100, r.data() + done + 100, chunk - 100, 1.0f, 1.0f);
            }
            std::vector<float> o0 ((std::size_t) n), o1 ((std::size_t) n), o2 ((std::size_t) n), o3 ((std::size_t) n);
            float* outs[] { o0.data(), o1.data(), o2.data(), o3.data() };
            dev.bus.drainInto (outs, 4, [] (int c) { return c < 4 ? c : -1; }, n);
            dev.outA.insert (dev.outA.end(), o2.begin(), o2.end());
            dev.outB.insert (dev.outB.end(), o3.begin(), o3.end());
            for (int i = 0; i < n; ++i)
                energy += (double) l[(std::size_t) i] * l[(std::size_t) i];
        }
        if (withDry)
            CHECK (energy < 1e-8);
        else
            CHECK (energy > 1.0);
    }
}

TEST (patch165_round_trip_is_structure_inside_a_patcher)
{
    using namespace lynx;
    // a new round trip (the ping's result) builds the graph again, so its delay compensation and the reported latency
    // follow; the ping button and the gains are values the running graph takes
    const auto a = patch165::parallelSlot (300);
    auto b = a;
    b.patcher.nodes[0].effect.params[hwinsert::ping] = 1.0f;
    b.patcher.nodes[0].effect.params[hwinsert::send] = 0.3f;
    CHECK (patcher::sameStructure (a.patcher, b.patcher));
    b.patcher.nodes[0].effect.params[hwinsert::roundTrip] = hwinsert::roundTripValue (301);
    CHECK (! patcher::sameStructure (a.patcher, b.patcher));
    const auto f = patcher::builtInFactories (48000.0);
    auto first = patcher::makeEffect (a, 48000.0, f);
    auto second = patcher::makeEffect (b, 48000.0, f, first.get());
    CHECK (first->latencySamples() == 300 && second->latencySamples() == 301);
}

TEST (patch165_song_finds_hardware_inserts_inside_patchers)
{
    using namespace lynx;
    Song song = Song::makeDefault();
    CHECK (hwinsert::countActive (song) == 0);
    // a mixer-slot Patcher, a Patcher inside it, a generator Patcher
    auto slot = patch165::parallelSlot (100);
    auto nested = lynx::makeEffectSlot (EffectKind::patcher);
    nested.patcher.nodes = { patch165::hwNode (50) };
    PatchNode holder;
    holder.effect = nested;
    slot.patcher.nodes.push_back (holder);
    CHECK (patcher::usesHardwareIo (nested.patcher) && patcher::usesHardwareIo (slot.patcher));
    song.mixer[1].effects.push_back (slot);
    CHECK (hwinsert::countActive (song) == 2 && hwinsert::returnsFromInput (song));
    ChannelDef gen;
    gen.instrument = InstrumentKind::patcher;
    gen.patcher.nodes = { patch165::hwNode (10) };
    song.channels.push_back (gen);
    CHECK (hwinsert::countActive (song) == 3);
    song.mixer[1].effects.back().enabled = false; // (the Patcher slot off: the two inside it are off)
    CHECK (hwinsert::countActive (song) == 1);
    // the rack runs both on the audio thread (the device-output buses are not shared between threads)
    song.mixer[1].effects.back().enabled = true;
    auto state = RackState::build (song, nullptr, 48000.0);
    auto pinnedTrack = false;
    for (const auto& layer : state->audioThreadLayers)
        pinnedTrack = pinnedTrack || std::find (layer.begin(), layer.end(), 1) != layer.end();
    CHECK (pinnedTrack);
    const auto c = (int) song.channels.size() - 1;
    CHECK (std::find (state->audioThreadChannels.begin(), state->audioThreadChannels.end(), c) != state->audioThreadChannels.end());
    // and plans the Patcher's latency (its wired insert: 100)
    CHECK (state->pdc.total >= 100);
}

TEST (patch165_keyboard_controller_state_round_trip)
{
    using namespace lynx;
    // the editor's writes keep FL's other bytes (smoothing, key range, link) and read back exactly
    std::vector<std::uint8_t> state;
    KeyboardControllerData d;
    for (std::size_t k = 0; k < d.keys.size(); ++k)
        d.keys[k] = (float) (k % 7) / 6.0f;
    d.noteOffValue = 0.25f;
    d.autoRelease = true;
    writeKeyboardController (d, state); // (from nothing: a new version 3 state)
    CHECK (state.size() == 554);
    auto back = parseKeyboardController (state);
    CHECK (back && back->keys == d.keys && back->noteOffValue == 0.25f && back->autoRelease);
    state[4] = 77;   // FL's attack
    state[541] = 9;  // the UI's key range
    d.autoRelease = false;
    d.keys[60] = 2.0f; // (clamped)
    writeKeyboardController (d, state);
    back = parseKeyboardController (state);
    CHECK (state[4] == 77 && state[541] == 9 && back && ! back->autoRelease && back->keys[60] == 1.0f);
}

// --- Round 165c patch165: FL's Layer as a Patcher node (its Children pin plays the generators cabled to it)

namespace patch165
{
// What reaches a child: each note with the level, pan and pitch it was given
class LayerRecorder final : public lynx::Instrument
{
public:
    explicit LayerRecorder (bool scalesVoices) : scales (scalesVoices) {}
    void prepare (double) noexcept override {}
    void noteOn (int k, float v) noexcept override { ons.push_back ({ k, v, nextGain, nextPan, nextCents }), nextGain = 1.0f, nextPan = nextCents = 0.0f; }
    void noteOff (int k) noexcept override { offs.push_back (k); }
    void allNotesOff() noexcept override {}
    void render (float*, float*, int, float, float) noexcept override {}
    void setNextNoteExpression (float pan, float cents, bool) noexcept override { nextPan = pan, nextCents = cents; }
    bool setNextNoteGain (float g) noexcept override
    {
        if (scales)
            nextGain = g;
        return scales;
    }
    void pitchBend (float) noexcept override { ++bends; }
    struct On
    {
        int key;
        float velocity, gain, pan, cents;
    };
    std::vector<On> ons;
    std::vector<int> offs;
    int bends = 0;
    bool scales;
    float nextGain = 1.0f, nextPan = 0.0f, nextCents = 0.0f;
};

// A Patcher: host notes -> Layer (node 0) -> children (nodes 1…), each a recorder named "c<k>"
struct LayerRig
{
    bool scales = true;
    lynx::PatcherDoc doc;
    std::map<std::string, std::shared_ptr<LayerRecorder>> made;
    std::unique_ptr<lynx::patcher::Graph> graph;

    explicit LayerRig (int children, bool scalesVoices = true) : scales (scalesVoices)
    {
        lynx::PatchNode layer;
        layer.generator = true;
        layer.channel.instrument = lynx::InstrumentKind::layer;
        layer.channel.name = "layer";
        doc.nodes.push_back (layer);
        doc.wires.push_back (patcher162::midi (lynx::patchFromHost, 0, 0, 0));
        for (int k = 1; k <= children; ++k)
        {
            lynx::PatchNode c;
            c.generator = true;
            c.channel.instrument = lynx::InstrumentKind::synth;
            c.channel.name = "c" + std::to_string (k);
            doc.nodes.push_back (c);
            doc.wires.push_back (patcher162::midi (0, 0, k, 0));
        }
    }
    void build()
    {
        made.clear();
        lynx::patcher::Factories f;
        f.instrument = [this] (const lynx::ChannelDef& c) -> std::shared_ptr<lynx::Instrument> {
            auto r = std::make_shared<LayerRecorder> (scales);
            made[c.name] = r;
            return r;
        };
        graph = std::make_unique<lynx::patcher::Graph> (doc, 48000.0, f);
        graph->prepare (48000.0);
    }
    LayerRecorder& child (int k) { return *made["c" + std::to_string (k)]; }
    lynx::ChannelDef& layer() { return doc.nodes[0].channel; }
};
} // namespace patch165

TEST (patch165_layer_node_plays_every_child_at_its_level)
{
    using namespace lynx;
    patch165::LayerRig rig (2);
    rig.build();
    rig.graph->noteOn (60, 0.8f);
    rig.graph->noteOff (60);
    for (int k = 1; k <= 2; ++k)
    {
        auto& c = rig.child (k);
        CHECK (c.ons.size() == 1 && c.ons[0].key == 60 && c.ons[0].velocity == 0.8f);
        // FL multiplies the child voice by the Layer's channel gain (the default 78 %: about −5 dB)
        CHECK (std::abs (c.ons[0].gain - channelGain (0.78f)) < 1e-6f && c.ons[0].pan == 0.0f && c.ons[0].cents == 0.0f);
        CHECK (c.offs == std::vector<int> { 60 });
    }
    CHECK (rig.made["layer"]->ons.empty()); // (the Layer makes no sound of its own)
    // pitch bend reaches the children
    rig.graph->pitchBend (1.0f);
    CHECK (rig.child (1).bends == 1 && rig.child (2).bends == 1);
    // an instrument that does not scale its voices gets the level on the velocity
    patch165::LayerRig plain (1, false);
    plain.build();
    plain.graph->noteOn (60, 0.8f);
    CHECK (plain.child (1).ons.size() == 1 && std::abs (plain.child (1).ons[0].velocity - 0.8f * channelGain (0.78f)) < 1e-6f);
}

TEST (patch165_layer_node_pan_pitch_and_key_regions)
{
    using namespace lynx;
    patch165::LayerRig rig (2);
    rig.layer().pan = -0.5f;
    rig.layer().misc.rootNote = 56;   // (its children play 4 semitones up, as RawFL's Layer)
    rig.layer().sampler.pitch = 0.5f; // + its PITCH knob
    rig.layer().volume = 1.0f;
    rig.doc.nodes[2].channel.keyLow = rig.doc.nodes[2].channel.keyHigh = 64; // child 2: only E5
    rig.build();
    rig.graph->noteOn (60, 1.0f);
    rig.graph->noteOn (64, 1.0f);
    CHECK (rig.child (1).ons.size() == 2 && rig.child (2).ons.size() == 1 && rig.child (2).ons[0].key == 64);
    CHECK (rig.child (1).ons[0].pan == -0.5f && std::abs (rig.child (1).ons[0].cents - 450.0f) < 1e-3f && rig.child (1).ons[0].gain == 1.0f);
}

TEST (patch165_layer_node_crossfade_sequential_random)
{
    using namespace lynx;
    { // Crossfade at 0: only the first child sounds; at 0.5 of 3 children: the middle one fully
        patch165::LayerRig rig (3);
        rig.layer().layer.crossfade = true;
        rig.layer().volume = 1.0f;
        rig.build();
        rig.graph->noteOn (60, 1.0f);
        CHECK (rig.child (1).ons.size() == 1 && rig.child (2).ons.empty() && rig.child (3).ons.empty());
        rig.layer().layer.fade = 0.5f;
        rig.build();
        rig.graph->noteOn (60, 1.0f);
        CHECK (rig.child (1).ons.empty() && rig.child (2).ons.size() == 1 && rig.child (3).ons.empty() && rig.child (2).ons[0].gain == 1.0f);
        rig.layer().layer.fade = 0.25f; // halfway between children 1 and 2
        rig.build();
        rig.graph->noteOn (60, 1.0f);
        CHECK (rig.child (1).ons.size() == 1 && rig.child (2).ons.size() == 1 && std::abs (rig.child (1).ons[0].gain - 0.5f) < 1e-6f);
    }
    { // Sequential: the children in turn; each note off goes to the child its note on played
        patch165::LayerRig rig (3);
        rig.layer().layer.sequential = true;
        rig.build();
        for (int n = 0; n < 6; ++n)
        {
            rig.graph->noteOn (60 + n, 1.0f);
            rig.graph->noteOff (60 + n);
        }
        for (int k = 1; k <= 3; ++k)
        {
            auto& c = rig.child (k);
            CHECK (c.ons.size() == 2 && c.ons[0].key == 60 + (k - 1) && c.ons[1].key == 63 + (k - 1));
            CHECK (c.offs == (std::vector<int> { 60 + (k - 1), 63 + (k - 1) }));
        }
    }
    { // Random: one child per note, spread over the children, offs on the same child
        patch165::LayerRig rig (2);
        rig.layer().layer.random = true;
        rig.build();
        for (int n = 0; n < 200; ++n)
        {
            rig.graph->noteOn (60, 1.0f);
            rig.graph->noteOff (60);
        }
        const auto a = rig.child (1).ons.size(), b = rig.child (2).ons.size();
        CHECK (a + b == 200 && a > 60 && b > 60);
        CHECK (rig.child (1).offs.size() == a && rig.child (2).offs.size() == b);
    }
}

TEST (patch165_layer_node_children_and_structure)
{
    using namespace lynx;
    // children: what the Children pin reaches through MIDI Out nodes, Layers left out, in node order
    patch165::LayerRig rig (2);
    PatchNode out, other;
    out.generator = other.generator = true;
    out.channel.instrument = InstrumentKind::midiOut;
    midictl::MidiOutSettings settings;
    settings.channel = 5;
    midictl::writeMidiOut (out.channel.pluginState, settings);
    other.channel.instrument = InstrumentKind::layer;
    rig.doc.nodes.push_back (out);   // 3
    rig.doc.nodes.push_back (other); // 4
    PatchNode late;
    late.generator = true;
    late.channel.instrument = InstrumentKind::synth;
    late.channel.name = "c5";
    rig.doc.nodes.push_back (late); // 5
    rig.doc.wires.push_back (patcher162::midi (0, 0, 3, 0));
    rig.doc.wires.push_back (patcher162::midi (3, 0, 5, 0));
    rig.doc.wires.push_back (patcher162::midi (0, 0, 4, 0));
    CHECK (patcher::layerChildren (rig.doc, 0) == (std::vector<int> { 1, 2, 5 }));
    CHECK (patcher::nodePorts (rig.doc.nodes[0]).midiOut && patcher::nodePorts (rig.doc.nodes[0]).audioOuts == 0);
    // its settings are structure (the graph's note routes): a change builds the graph again; a child's key region too
    auto b = rig.doc;
    b.nodes[0].channel.layer.random = true;
    CHECK (! patcher::sameStructure (rig.doc, b));
    b = rig.doc;
    b.nodes[1].channel.keyLow = 40;
    CHECK (! patcher::sameStructure (rig.doc, b));
    // Split children on nodes: one key each from the Layer's root, Add to key so each sounds its own pitch there
    std::vector<ChannelDef*> kids;
    for (const auto k : patcher::layerChildren (rig.doc, 0))
        kids.push_back (&rig.doc.nodes[(std::size_t) k].channel);
    rig.layer().misc.rootNote = 48;
    layer::splitChildren (rig.layer(), kids);
    CHECK (rig.doc.nodes[1].channel.keyLow == 48 && rig.doc.nodes[2].channel.keyHigh == 49 && rig.doc.nodes[5].channel.keyLow == 50);
    rig.build();
    rig.graph->noteOn (49, 1.0f); // child 2's key: it plays at its own pitch (C5), through Add to key
    CHECK (rig.child (1).ons.empty() && rig.child (2).ons.size() == 1 && rig.child (2).ons[0].key == 60);
    rig.graph->noteOff (49);
    CHECK (rig.child (2).offs == std::vector<int> { 60 });
    layer::resetChildren (kids);
    CHECK (! rig.doc.nodes[1].channel.hasKeyRegion() && ! rig.doc.nodes[1].channel.misc.addToKey);
}
