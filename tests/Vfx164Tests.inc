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

TEST (vfx_key_mapper_follows_fl_base_key_offset_and_cycles)
{
    using namespace vfx164;
    using lynx::vfx::KeyMapperSettings;
    auto minor = lynx::vfx::defaultKeyMapper();
    // FL's Chords / Minor preset rows
    const std::vector<std::vector<int>> rows { { 0, 3, 7 },   { 0, 3, 5 },   { 2, 5, 8 },    { 3, 7, 8 },    { 3, 7, 10 },   { 5, 8, 12 },
                                               { 5, 8, 10 },  { 7, 10, 14 }, { 8, 12, 14 },  { 8, 12, 15 },  { 10, 14, 15 }, { 10, 14, 17 } };
    for (int r = 0; r < 12; ++r)
        minor.rows[(std::size_t) r] = rows[(std::size_t) r];
    auto keys = [] (const std::vector<lynx::vfx::Event>& out) {
        std::vector<int> k;
        for (const auto& e : out)
            if (e.type == lynx::vfx::Event::Type::on)
                k.push_back (e.voice.key);
        std::sort (k.begin(), k.end());
        return k;
    };
    auto p = lynx::vfx::makeProcessor (lynx::vfx::Kind::keyMapper, lynx::vfx::writeKeyMapper (minor));
    CHECK ((keys (run (*p, { on (60) })) == std::vector<int> { 60, 63, 67 }));
    CHECK ((keys (run (*p, { on (63) })) == std::vector<int> { 63, 67, 68 }));
    CHECK ((keys (run (*p, { on (71) })) == std::vector<int> { 70, 74, 77 }));
    // base key D: 62 plays row 0 from D, 60 is row 10 of the octave starting on 50
    minor.baseKey = 2;
    p = lynx::vfx::makeProcessor (lynx::vfx::Kind::keyMapper, lynx::vfx::writeKeyMapper (minor));
    CHECK ((keys (run (*p, { on (62) })) == std::vector<int> { 62, 65, 69 }));
    CHECK ((keys (run (*p, { on (60) })) == std::vector<int> { 60, 64, 65 }));
    // the offset transposes what comes out (it does not pick another row)
    auto identity = lynx::vfx::defaultKeyMapper();
    identity.offset = -12;
    p = lynx::vfx::makeProcessor (lynx::vfx::Kind::keyMapper, lynx::vfx::writeKeyMapper (identity));
    CHECK ((keys (run (*p, { on (60) })) == std::vector<int> { 48 }));
    // cycle up / down follow the row's own order
    auto cyc = lynx::vfx::defaultKeyMapper();
    cyc.rows[0] = { 7, 0, 3 };
    cyc.multimode = 3;
    p = lynx::vfx::makeProcessor (lynx::vfx::Kind::keyMapper, lynx::vfx::writeKeyMapper (cyc));
    std::vector<int> seq;
    for (int k = 0; k < 4; ++k)
    {
        seq.push_back (keys (run (*p, { on (60) }))[0]);
        run (*p, { off (60) });
    }
    CHECK ((seq == std::vector<int> { 67, 60, 63, 67 }));
    cyc.multimode = 4;
    p = lynx::vfx::makeProcessor (lynx::vfx::Kind::keyMapper, lynx::vfx::writeKeyMapper (cyc));
    seq.clear();
    for (int k = 0; k < 4; ++k)
    {
        seq.push_back (keys (run (*p, { on (60) }))[0]);
        run (*p, { off (60) });
    }
    CHECK ((seq == std::vector<int> { 63, 60, 67, 63 }));
    // an empty row mutes the note
    auto muted = lynx::vfx::defaultKeyMapper();
    muted.rows[7].clear();
    p = lynx::vfx::makeProcessor (lynx::vfx::Kind::keyMapper, lynx::vfx::writeKeyMapper (muted));
    CHECK (run (*p, { on (67) }).empty() && ! p->busy());
}

TEST (vfx_key_mapper_pitch_slide_glides_to_the_new_note)
{
    using namespace vfx164;
    auto down = lynx::vfx::defaultKeyMapper();
    for (int r = 0; r < 12; ++r)
        down.rows[(std::size_t) r] = { r - 12 };
    down.slide = 144; // FL's "Slide down": 3 steps
    auto p = lynx::vfx::makeProcessor (lynx::vfx::Kind::keyMapper, lynx::vfx::writeKeyMapper (down));
    lynx::vfx::EventList out;
    lynx::vfx::Transport t;
    t.sampleRate = 48000.0;
    t.bpm = 120.0; // a step = 0.125 s = 6000 samples; 3 steps = 18000
    std::vector<lynx::vfx::Event> in { on (60) };
    p->process (in.data(), 1, 1000, t, out);
    CHECK (out.size() == 1 && out[0].voice.key == 48 && std::abs (out[0].voice.cents - 1200.0f) < 0.01f);
    float last = 1200.0f;
    int steps = 0;
    for (int b = 1; b * 1000 <= 18000; ++b)
    {
        out.clear();
        p->process (nullptr, 0, 1000, t, out);
        CHECK (out.size() == 1 && out[0].type == lynx::vfx::Event::Type::prop && out[0].prop == lynx::vfx::Prop::pitch);
        if (out.size() == 1)
        {
            CHECK (out[0].value < last);
            last = out[0].value;
        }
        ++steps;
    }
    CHECK (steps == 18 && last < 1200.0f * 0.02f); // ~99 % of the way after 3 steps
}

TEST (vfx_level_scaler_curves_match_fl)
{
    using L = lynx::vfx::LevelScalerSettings;
    auto s = lynx::vfx::defaultLevelScaler();
    const auto fl = lynx::vfx::writeLevelScaler (s);
    CHECK (fl.size() == 173);
    CHECK (lynx::vfx::parseLevelScaler (fl) == s);
    CHECK (lynx::vfx::levelScale (s, L::velocity, 0.3, 0.0, 1.0, 0.5) == 0.3); // identity
    auto near = [] (double a, double b, double eps = 1e-4) { return std::abs (a - b) < eps; };
    s.at (L::velocity, L::multiply) = 6842; // AutoBeat Lite
    CHECK (near (lynx::vfx::levelScale (s, L::velocity, 0.78125, 0.0, 1.0, 0.0), 0.652504));
    s = lynx::vfx::defaultLevelScaler();
    s.at (L::velocity, L::tension) = 8192;
    CHECK (near (lynx::vfx::levelScale (s, L::velocity, 0.5, 0.0, 1.0, 0.0), 0.989073));
    s.at (L::velocity, L::tension) = -8192;
    CHECK (near (lynx::vfx::levelScale (s, L::velocity, 0.5, 0.0, 1.0, 0.0), 0.010927));
    s = lynx::vfx::defaultLevelScaler();
    s.at (L::finePitch, L::multiply) = 4096;
    CHECK (near (lynx::vfx::levelScale (s, L::finePitch, 0.5, -1.0, 1.0, 0.0) * 1200.0, 300.0, 0.01));
    CHECK (near (lynx::vfx::levelScale (s, L::finePitch, -1.0, -1.0, 1.0, 0.0) * 1200.0, -600.0, 0.01));
    s = lynx::vfx::defaultLevelScaler();
    s.at (L::pan, L::offset) = 4096;
    CHECK (near (lynx::vfx::levelScale (s, L::pan, 0.0, -1.0, 1.0, 0.0), 1.0));
    CHECK (near (lynx::vfx::levelScale (s, L::pan, -1.0, -1.0, 1.0, 0.0), 0.0));
    s = lynx::vfx::defaultLevelScaler();
    s.at (L::modX, L::multiply) = -8192;
    CHECK (near (lynx::vfx::levelScale (s, L::modX, 0.4, -1.0, 1.0, 0.0), -0.4));
    // humanize: spread around the value, subtract below, add above
    s = lynx::vfx::defaultLevelScaler();
    s.at (L::velocity, L::humanize) = 8192;
    CHECK (near (lynx::vfx::levelScale (s, L::velocity, 0.5, 0.0, 1.0, 0.0), 0.25) && near (lynx::vfx::levelScale (s, L::velocity, 0.5, 0.0, 1.0, 1.0), 0.75));
    s.at (L::velocity, L::humanizeMode) = 0;
    CHECK (near (lynx::vfx::levelScale (s, L::velocity, 0.5, 0.0, 1.0, 1.0), 0.5));
    s.at (L::velocity, L::humanizeMode) = 2;
    CHECK (near (lynx::vfx::levelScale (s, L::velocity, 0.5, 0.0, 1.0, 0.0), 0.5));
    // the processor: a fixed velocity
    auto fixed = lynx::vfx::defaultLevelScaler();
    fixed.at (L::velocity, L::multiply) = 0;
    fixed.at (L::velocity, L::offset) = 4096;
    auto p = lynx::vfx::makeProcessor (lynx::vfx::Kind::levelScaler, lynx::vfx::writeLevelScaler (fixed));
    const auto out = vfx164::run (*p, { vfx164::on (60, 0, 0, 0.9f), vfx164::on (64, 0, 0, 0.2f) });
    CHECK (out.size() == 2 && near (out[0].voice.velocity, 0.5) && near (out[1].voice.velocity, 0.5) && out[0].voice.key == 60);
}
