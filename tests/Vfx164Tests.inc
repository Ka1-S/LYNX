// Round 164b: FL's VFX note processors (core/Vfx.h) in a Patcher and in the Channel Rack

#include "core/Vfx.h"

namespace vfx164
{
std::vector<std::uint8_t> hex (const char* h)
{
    std::vector<std::uint8_t> out;
    for (; h[0] != 0 && h[1] != 0; h += 2)
        out.push_back ((std::uint8_t) std::stoi (std::string (h, 2), nullptr, 16));
    return out;
}

// FL's factory presets (the 0xD5 record of Data/Patches/Plugin presets/Effects/VFX Key Mapper/…)
const char* keyMapperDefault = "020000000100000000000000000000000000000000000000010000000000010000000100010000000200010000000300010000000400010000000500010000000600010000000700010000000800010000000900010000000a00010000000b00";
const char* keyMapperOctaveUp = "0200000001000000000000000000000000000000000000000200000000000c000200000001000d000200000002000e000200000003000f00020000000400100002000000050011000200000006001200020000000700130002000000080014000200000009001500020000000a001600020000000b001700";
const char* keyMapperMajor = "020000000100000000000000000000000000000000000000010000000000010000000000010000000200010000000200010000000400010000000500010000000500010000000700010000000700010000000900010000000900010000000b00";
const char* colorMapperDefault = "00000000000000000100000002000000030000000400000005000000060000000700000008000000090000000a0000000b0000000c0000000d0000000e0000000f000000000000000100000002000000030000000400000005000000060000000700000008000000090000000a0000000b0000000c0000000d0000000e0000000f000000";

lynx::vfx::Event on (int key, int offset = 0, int colour = 0, float velocity = 0.8f)
{
    lynx::vfx::Event e;
    e.type = lynx::vfx::Event::Type::on;
    e.offset = offset;
    e.voice.key = key;
    e.voice.velocity = velocity;
    e.voice.colour = colour;
    e.id = lynx::vfx::inputId (key, colour);
    return e;
}

lynx::vfx::Event off (int key, int offset = 0, int colour = 0)
{
    auto e = on (key, offset, colour, 0.0f);
    e.type = lynx::vfx::Event::Type::off;
    return e;
}

// runs one block and returns what came out, sorted
std::vector<lynx::vfx::Event> run (lynx::vfx::Processor& p, std::vector<lynx::vfx::Event> in, int n = 512)
{
    lynx::vfx::EventList out;
    lynx::vfx::Transport t;
    p.process (in.data(), (int) in.size(), n, t, out);
    out.sortByOffset();
    std::vector<lynx::vfx::Event> v;
    for (int k = 0; k < out.size(); ++k)
        v.push_back (out[k]);
    return v;
}

// An instrument that writes down what it was asked to play, and at which sample of its renders
struct Probe final : lynx::Instrument
{
    struct Hit
    {
        bool on = true;
        int key = 0, sample = 0, channel = -1;
        float velocity = 0.0f;
    };
    std::vector<Hit> hits;
    int position = 0;
    void prepare (double) noexcept override {}
    void noteOn (int key, float velocity) noexcept override { hits.push_back ({ true, key, position, -1, velocity }); }
    void noteOff (int key) noexcept override { hits.push_back ({ false, key, position, -1, 0.0f }); }
    void noteOnMidi (int key, float velocity, int channel) noexcept override { hits.push_back ({ true, key, position, channel, velocity }); }
    void noteOffMidi (int key, int channel) noexcept override { hits.push_back ({ false, key, position, channel, 0.0f }); }
    void allNotesOff() noexcept override {}
    void render (float*, float*, int n, float, float) noexcept override { position += n; }
};

// A generator Patcher: Notes → VFX node (kind, state) → output `output` → a probe synth node (index 1)
struct Rig
{
    lynx::ChannelDef channel;
    std::vector<std::shared_ptr<Probe>> probes;
    std::unique_ptr<lynx::Instrument> instrument;

    Rig (lynx::vfx::Kind kind, std::vector<std::uint8_t> fl, int outputs = 1)
    {
        channel.instrument = lynx::InstrumentKind::patcher;
        lynx::PatchNode v;
        v.generator = true;
        lynx::vfx::makeChannel (v.channel, kind);
        lynx::vfx::State s;
        s.kind = kind;
        s.fl = std::move (fl);
        v.channel.pluginState = lynx::vfx::writeState (s);
        channel.patcher.nodes.push_back (v);
        channel.patcher.wires.push_back (patcher162::midi (lynx::patchFromHost, 0, 0, 0));
        for (int k = 0; k < outputs; ++k)
        {
            channel.patcher.nodes.push_back (patcher162::generatorNode (lynx::InstrumentKind::synth));
            channel.patcher.wires.push_back (patcher162::midi (0, k, 1 + k, 0));
        }
        auto factories = lynx::patcher::builtInFactories (48000.0);
        auto builtIn = factories.instrument;
        factories.instrument = [this, builtIn] (const lynx::ChannelDef& c) -> std::shared_ptr<lynx::Instrument> {
            if (c.instrument == lynx::InstrumentKind::synth)
            {
                probes.push_back (std::make_shared<Probe>());
                return probes.back();
            }
            return builtIn (c);
        };
        instrument = lynx::patcher::makeInstrument (channel, 48000.0, factories);
    }
    void render (int n)
    {
        std::vector<float> l ((std::size_t) n), r ((std::size_t) n);
        instrument->beginChunk (n);
        instrument->render (l.data(), r.data(), n, 1.0f, 1.0f);
    }
};
} // namespace vfx164

TEST (vfx_names_and_state_round_trip)
{
    using namespace lynx::vfx;
    for (const auto k : allKinds)
    {
        CHECK (kindOfFlName (flName (k)) == k);
        State s;
        s.kind = k;
        s.port = 7;
        s.fl = { 1, 2, 3 };
        const auto back = readState (writeState (s));
        CHECK (back && back->kind == k && back->port == 7 && back->fl == s.fl);
    }
    CHECK (kindOfFlName ("vfx key mapper") == Kind::keyMapper);
    CHECK (! kindOfFlName ("Fruity Delay 3"));
    CHECK (! readState ({ 1, 2, 3, 4, 5 }));
}

TEST (vfx_key_mapper_reads_fl_presets_and_writes_them_back)
{
    using namespace vfx164;
    for (const auto* h : { keyMapperDefault, keyMapperOctaveUp, keyMapperMajor })
    {
        const auto fl = hex (h);
        const auto s = lynx::vfx::parseKeyMapper (fl);
        CHECK (s.has_value());
        if (s)
            CHECK (lynx::vfx::writeKeyMapper (*s) == fl); // (byte for byte)
    }
    const auto up = lynx::vfx::parseKeyMapper (hex (keyMapperOctaveUp));
    CHECK (up && up->rows[3].size() == 2 && up->rows[3][0] == 3 && up->rows[3][1] == 15);
    CHECK (lynx::vfx::writeKeyMapper (lynx::vfx::defaultKeyMapper()) == hex (keyMapperDefault));
}

TEST (vfx_key_mapper_maps_and_releases_what_it_played)
{
    using namespace vfx164;
    auto p = lynx::vfx::makeProcessor (lynx::vfx::Kind::keyMapper, hex (keyMapperOctaveUp));
    auto out = run (*p, { on (62, 10) });
    CHECK (out.size() == 2 && out[0].voice.key == 62 && out[1].voice.key == 74 && out[0].offset == 10);
    CHECK (p->busy());
    // the settings change while the note is held: its "off" still ends exactly what it started
    p->configure (hex (keyMapperDefault));
    out = run (*p, { off (62, 3) });
    CHECK (out.size() == 2 && out[0].type == lynx::vfx::Event::Type::off && out[1].type == lynx::vfx::Event::Type::off);
    CHECK (out.size() == 2 && ((out[0].voice.key == 62 && out[1].voice.key == 74) || (out[0].voice.key == 74 && out[1].voice.key == 62)));
    CHECK (! p->busy());
    // C major: C# plays C
    auto major = lynx::vfx::makeProcessor (lynx::vfx::Kind::keyMapper, hex (keyMapperMajor));
    out = run (*major, { on (61) });
    CHECK (out.size() == 1 && out[0].voice.key == 60);
}

TEST (vfx_color_mapper_sends_each_colour_to_its_voice_port)
{
    using namespace vfx164;
    auto s = lynx::vfx::parseColorMapper (hex (colorMapperDefault));
    CHECK (s && *s == lynx::vfx::defaultColorMapper());
    CHECK (lynx::vfx::writeColorMapper (lynx::vfx::defaultColorMapper()) == hex (colorMapperDefault));
    auto changed = lynx::vfx::defaultColorMapper();
    changed.port[5] = 1;
    changed.colour[5] = 0;
    auto p = lynx::vfx::makeProcessor (lynx::vfx::Kind::colorMapper, lynx::vfx::writeColorMapper (changed));
    const auto out = run (*p, { on (60, 0, 2), on (64, 0, 5) });
    CHECK (out.size() == 2 && out[0].output == 2 && out[0].voice.colour == 2);
    CHECK (out.size() == 2 && out[1].output == 1 && out[1].voice.colour == 0);
}

TEST (vfx_node_in_a_patcher_plays_its_instruments)
{
    using namespace vfx164;
    Rig rig (lynx::vfx::Kind::keyMapper, hex (keyMapperOctaveUp));
    CHECK (rig.probes.size() == 1);
    rig.render (64);
    rig.instrument->noteOn (60, 0.5f);
    rig.render (64);
    rig.instrument->noteOff (60);
    rig.render (64);
    const auto& hits = rig.probes[0]->hits;
    CHECK (hits.size() == 4);
    if (hits.size() == 4)
    {
        CHECK (hits[0].on && hits[1].on && hits[0].key + hits[1].key == 60 + 72);
        CHECK (hits[0].sample == 64 && hits[0].velocity == 0.5f);
        CHECK (! hits[2].on && ! hits[3].on && hits[2].sample == 128);
    }
    // its kind is structure, its settings are not: a new state keeps the graph running
    auto other = rig.channel;
    lynx::vfx::State s { lynx::vfx::Kind::keyMapper, 0, hex (keyMapperDefault) };
    other.patcher.nodes[0].channel.pluginState = lynx::vfx::writeState (s);
    CHECK (lynx::patcher::sameStructure (rig.channel.patcher, other.patcher));
}

TEST (vfx_color_mapper_node_splits_colours_over_its_outputs)
{
    using namespace vfx164;
    Rig rig (lynx::vfx::Kind::colorMapper, hex (colorMapperDefault), 3);
    CHECK (rig.probes.size() == 3);
    rig.instrument->setNextNoteColour (2);
    rig.instrument->noteOn (60, 0.5f);
    rig.instrument->noteOn (62, 0.5f); // colour 0
    rig.render (32);
    CHECK (rig.probes.size() == 3 && rig.probes[2]->hits.size() == 1 && rig.probes[2]->hits[0].key == 60 && rig.probes[2]->hits[0].channel == 2);
    CHECK (rig.probes.size() == 3 && rig.probes[0]->hits.size() == 1 && rig.probes[0]->hits[0].key == 62);
    CHECK (rig.probes.size() == 3 && rig.probes[1]->hits.empty());
    const auto ports = lynx::patcher::nodePorts (rig.channel.patcher.nodes[0]);
    CHECK (ports.midiIn && ports.midiOuts == 16 && ports.audioOuts == 0);
    CHECK (lynx::patcher::shownMidiOutputs (rig.channel.patcher, 0) == 4);
}
