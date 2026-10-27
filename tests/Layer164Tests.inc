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
    CHECK (crossfadeGain (0, 1, 0.7f) == 1.0f);
    // a crossfaded Layer only plays the children with a level
    layer164::Rig rig (3, [] (lynx::Song& s) { s.channels[0].layer.crossfade = true; s.channels[0].layer.fade = 1.0f; });
    rig.play (0, 60);
    CHECK (rig.spies[1]->ons() == 0 && rig.spies[2]->ons() == 0 && rig.spies[3]->ons() == 1);
    // VOL at the default leaves the velocity alone (a plugin has no voice gain: the velocity carries it)
    CHECK (std::abs (rig.spies[3]->got[0].velocity - 0.8f) < 1e-5f);
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
