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

TEST (vfx_keyboard_splitter_zones)
{
    using namespace vfx164;
    using S = lynx::vfx::KeyboardSplitterSettings;
    auto keys = [] (const std::vector<lynx::vfx::Event>& out) {
        std::vector<std::pair<int, int>> k; // (output, key)
        for (const auto& e : out)
            if (e.type == lynx::vfx::Event::Type::on)
                k.emplace_back (e.output, e.voice.key);
        std::sort (k.begin(), k.end());
        return k;
    };
    auto def = lynx::vfx::defaultKeyboardSplitter();
    CHECK (lynx::vfx::parseKeyboardSplitter (lynx::vfx::writeKeyboardSplitter (def)) == def);
    auto p = lynx::vfx::makeProcessor (lynx::vfx::Kind::keyboardSplitter, lynx::vfx::writeKeyboardSplitter (def));
    auto out = run (*p, { on (60, 0, 0, 0.78125f) });
    CHECK (out.size() == 1 && out[0].output == 0 && out[0].voice.key == 60 && std::abs (out[0].voice.velocity - 0.78125f) < 1e-4f);
    // FL's "3 zone split transposed"
    S split;
    auto zone = [] (double from, double to) {
        lynx::vfx::MapCurve m;
        m.points.push_back ({ 0.0, 0.0 });
        m.points.push_back ({ from / 132.0, 0.0 });
        m.points.push_back ({ from / 132.0, 1.0 });
        m.points.push_back ({ (to + 1) / 132.0, 1.0 });
        m.points.push_back ({ (to + 1) / 132.0, 0.0 });
        m.points.push_back ({ 1.0, 0.0 });
        return m;
    };
    split.zones[0].keyboard = zone (0, 47);
    split.zones[0].transpose = 36;
    split.zones[1].keyboard = zone (48, 83);
    split.zones[2].keyboard = zone (84, 131);
    split.zones[2].transpose = -36;
    p = lynx::vfx::makeProcessor (lynx::vfx::Kind::keyboardSplitter, lynx::vfx::writeKeyboardSplitter (split));
    CHECK ((keys (run (*p, { on (40) })) == std::vector<std::pair<int, int>> { { 0, 76 } }));
    CHECK ((keys (run (*p, { on (60) })) == std::vector<std::pair<int, int>> { { 1, 60 } }));
    CHECK ((keys (run (*p, { on (90) })) == std::vector<std::pair<int, int>> { { 2, 54 } }));
    out = run (*p, { off (40) });
    CHECK (out.size() == 1 && out[0].type == lynx::vfx::Event::Type::off && out[0].voice.key == 76 && out[0].output == 0);
    // Velocity / Limited (absolute): v 1 → map 0.75 → velocity 0.75 / 1.28
    S limited = lynx::vfx::defaultKeyboardSplitter();
    limited.zones[0].velocity.points = { { 0.0, 0.0 }, { 0.75, 0.75 }, { 1.0, 0.75 } };
    limited.zones[0].absolute = true;
    p = lynx::vfx::makeProcessor (lynx::vfx::Kind::keyboardSplitter, lynx::vfx::writeKeyboardSplitter (limited));
    out = run (*p, { on (60, 0, 0, 1.0f), on (62, 0, 0, 0.4f) });
    CHECK (out.size() == 2 && std::abs (out[0].voice.velocity - 0.5859f) < 1e-3f && std::abs (out[1].voice.velocity - 0.3125f) < 1e-3f);
    // random zones: each note on exactly one of them
    S rnd;
    rnd.zones[0].keyboard = rnd.zones[1].keyboard = lynx::vfx::MapCurve::flat (1.0);
    rnd.zones[0].random = rnd.zones[1].random = true;
    p = lynx::vfx::makeProcessor (lynx::vfx::Kind::keyboardSplitter, lynx::vfx::writeKeyboardSplitter (rnd));
    int counts[2] {};
    for (int k = 0; k < 200; ++k)
    {
        out = run (*p, { on (60), off (60, 1) });
        CHECK (out.size() == 2);
        if (! out.empty())
            ++counts[std::clamp (out[0].output, 0, 1)];
    }
    CHECK (counts[0] > 50 && counts[1] > 50);
}

TEST (vfx_reads_fl_factory_presets)
{
    if (realFlpDir.empty())
        return;
    namespace fs = std::filesystem;
    int files = 0;
    for (const auto kind : { lynx::vfx::Kind::keyMapper, lynx::vfx::Kind::keyboardSplitter, lynx::vfx::Kind::envelope })
    {
        const auto dir = fs::path (realFlpDir) / "Patches" / "Plugin presets" / "Effects" / lynx::vfx::flName (kind);
        if (! fs::is_directory (dir))
            continue;
        for (const auto& entry : fs::recursive_directory_iterator (dir))
        {
            if (entry.path().extension() != ".fst")
                continue;
            std::ifstream in (entry.path(), std::ios::binary);
            const std::vector<std::uint8_t> bytes ((std::istreambuf_iterator<char> (in)), {});
            const auto state = lynx::vfx::stateFromFlPreset (bytes, kind);
            CHECK (state.has_value());
            if (! state)
            {
                std::printf ("  not read: %s\n", entry.path().string().c_str());
                continue;
            }
            ++files;
            if (kind == lynx::vfx::Kind::keyMapper)
            {
                const auto s = lynx::vfx::parseKeyMapper (*state);
                CHECK (s && lynx::vfx::writeKeyMapper (*s) == *state);
            }
            if (kind == lynx::vfx::Kind::keyboardSplitter)
            {
                const auto s = lynx::vfx::parseKeyboardSplitter (*state);
                CHECK (s.has_value());
                if (s)
                {
                    const auto again = lynx::vfx::parseKeyboardSplitter (lynx::vfx::writeKeyboardSplitter (*s));
                    CHECK (again && again->zones == s->zones);
                }
            }
            if (kind == lynx::vfx::Kind::envelope)
            {
                const auto s = lynx::vfx::parseEnvelope (*state);
                CHECK (s.has_value());
                if (s)
                {
                    const auto again = lynx::vfx::parseEnvelope (lynx::vfx::writeEnvelope (*s));
                    CHECK (again && *again == *s);
                }
            }
        }
    }
    CHECK (files >= 50);
}

TEST (vfx_envelope_moves_the_notes_it_matches)
{
    using namespace vfx164;
    using E = lynx::vfx::EnvelopeSettings;
    auto def = lynx::vfx::defaultEnvelope();
    const auto again = lynx::vfx::parseEnvelope (lynx::vfx::writeEnvelope (def));
    CHECK (again && *again == def);
    auto p = lynx::vfx::makeProcessor (lynx::vfx::Kind::envelope, lynx::vfx::writeEnvelope (def));
    lynx::vfx::EventList out;
    lynx::vfx::Transport t;
    t.sampleRate = 48000.0;
    // colour 0: the volume envelope starts silent; colour 3 passes untouched
    std::vector<lynx::vfx::Event> in { on (60, 0, 0, 0.78125f), on (64, 0, 3, 0.5f) };
    p->process (in.data(), 2, 480, t, out);
    out.sortByOffset();
    CHECK (out.size() == 2 && out[0].voice.velocity < 0.01f && std::abs (out[1].voice.velocity - 0.5f) < 1e-5f);
    // 0.75 s in: the peak (volume × 2)
    float peak = 0.0f;
    for (int b = 0; b < 75; ++b)
    {
        out.clear();
        p->process (nullptr, 0, 480, t, out);
        for (int k = 0; k < out.size(); ++k)
            if (out[k].type == lynx::vfx::Event::Type::prop && out[k].prop == lynx::vfx::Prop::velocity)
                peak = std::max (peak, out[k].value);
    }
    const auto twice = (float) (std::log (10.0 * 2.0 + 1.0) / std::log (11.0) / 1.28); // velocity of volume × 2
    CHECK (std::abs (peak - twice) < 0.02f);
    CHECK (p->busy());
    // pitch: articulator 1's pitch envelope flat at the top, 100 %: +48 semitones
    auto up = lynx::vfx::defaultEnvelope();
    auto& pitch = up.articulators[0].parts[E::pitch];
    pitch.env = 1.0f;
    pitch.envelope.points = { { 0.0, 1.0 }, { 1.0, 1.0 } };
    pitch.envelope.footer[1] = -1;
    up.articulators[0].parts[E::velocity].envelope.footer[1] = 0; // (no volume envelope)
    up.articulators[0].low = 61; // keys 60…71 only
    up.articulators[0].high = 72;
    p = lynx::vfx::makeProcessor (lynx::vfx::Kind::envelope, lynx::vfx::writeEnvelope (up));
    const auto res = run (*p, { on (60), on (72) });
    CHECK (res.size() == 2 && std::abs (res[0].voice.cents - 4800.0f) < 0.5f && res[1].voice.cents == 0.0f);
    // the plugin off: everything passes
    up.enabled = false;
    p = lynx::vfx::makeProcessor (lynx::vfx::Kind::envelope, lynx::vfx::writeEnvelope (up));
    const auto off = run (*p, { on (60) });
    CHECK (off.size() == 1 && off[0].voice.cents == 0.0f);
}

namespace vfx164
{
// A sequencer note heard: its key, start and length in seconds, velocity (FL's 0…127, the volume × 100)
struct SeqNote
{
    int key = 0;
    double start = 0.0, length = -1.0;
    int velocity = 0;
};

// Plays `input` (seconds, key, on / off) through the processor at 120 bpm / 48 kHz with the song playing from 0
// (or stopped), and returns the notes it sent out
std::vector<SeqNote> sequence (lynx::vfx::Processor& p, std::vector<std::tuple<double, int, bool>> input, double seconds,
                               bool playing = true, float velocity = 100.0f / 128.0f)
{
    constexpr int block = 480;
    constexpr double rate = 48000.0;
    std::vector<SeqNote> notes;
    std::map<int, std::size_t> open;
    lynx::vfx::EventList out;
    lynx::vfx::Transport t;
    t.sampleRate = rate;
    t.bpm = 120.0;
    t.playing = playing;
    for (long long at = 0; at < (long long) (seconds * rate); at += block)
    {
        std::vector<lynx::vfx::Event> in;
        for (const auto& [time, key, down] : input)
        {
            const auto sample = (long long) std::llround (time * rate);
            if (sample >= at && sample < at + block)
            {
                auto e = down ? on (key, (int) (sample - at), 0, velocity) : off (key, (int) (sample - at));
                in.push_back (e);
            }
        }
        t.ticks = playing ? (double) at / rate * 2.0 * 960.0 : 0.0;
        out.clear();
        p.process (in.data(), (int) in.size(), block, t, out);
        out.sortByOffset();
        for (int k = 0; k < out.size(); ++k)
        {
            const auto& e = out[k];
            const auto time = (double) (at + e.offset) / rate;
            if (e.type == lynx::vfx::Event::Type::on)
            {
                const auto volume = (std::pow (11.0, 1.28 * (double) e.voice.velocity) - 1.0) / 10.0;
                open[e.id] = notes.size();
                notes.push_back ({ e.voice.key, time, -1.0, (int) std::lround (volume * 100.0) });
            }
            else if (e.type == lynx::vfx::Event::Type::off && open.count (e.id))
                notes[open[e.id]].length = time - notes[open[e.id]].start;
        }
    }
    return notes;
}

lynx::vfx::SequencerSettings blankProgram()
{
    auto s = lynx::vfx::defaultSequencer();
    s.set (lynx::vfx::SequencerSettings::currentProgram, 0); // "Initial": 16 × N1, gate 90 %, 1/16
    return s;
}

std::unique_ptr<lynx::vfx::Processor> sequencer (const lynx::vfx::SequencerSettings& s)
{
    return lynx::vfx::makeProcessor (lynx::vfx::Kind::sequencer, lynx::vfx::writeSequencer (s));
}
} // namespace vfx164

TEST (vfx_sequencer_state_round_trips_and_resolves_fl_params)
{
    using namespace lynx::vfx;
    using S = SequencerSettings;
    auto s = defaultSequencer();
    const auto bytes = writeSequencer (s);
    const auto back = parseSequencer (bytes);
    CHECK (back && *back == s && writeSequencer (*back) == bytes);
    CHECK (s.programs.size() == 128 && s.chains.size() == 16);
    // per program: Steps; global: Transpose
    s.set (S::currentProgram, 4);
    s.set (S::steps, 12);
    s.set (S::transposeSemitones, 5);
    s.set (S::currentProgram, 6);
    CHECK (s.get (S::steps) != 12 && s.get (S::transposeSemitones) == 5);
    s.set (S::currentProgram, 4);
    CHECK (s.get (S::steps) == 12);
    // lanes: FL params 0x80 + 64 · lane + step
    s.set (0x100 + 3, S::tie);
    CHECK (s.current().lanes[S::stepType][3] == S::tie && s.get (0x100 + 3) == S::tie);
    s.set (S::gate, 500);
    CHECK (s.get (S::gate) == 125);
    // pattern shift rotates the lanes of the steps in use
    s.set (S::steps, 16);
    s.current().lanes[S::velocity][0] = 127;
    shiftPattern (s, 1);
    CHECK (s.current().lanes[S::velocity][1] == 127);
    shiftPattern (s, -2);
    CHECK (s.current().lanes[S::velocity][15] == 127);
    CHECK (! parseSequencer ({ 1, 0, 0, 0 }));
    CHECK (defaultState (Kind::sequencer) == writeSequencer (defaultSequencer()));
}

TEST (vfx_sequencer_reads_fl_factory_bank)
{
    if (realFlpDir.empty())
        return;
    namespace fs = std::filesystem;
    const auto file = fs::path (realFlpDir) / "Patches" / "Plugin database" / "Effects" / "Patcher" / "VFX Sequencer.fst";
    if (! fs::exists (file))
        return;
    std::ifstream in (file, std::ios::binary);
    const std::vector<std::uint8_t> bytes ((std::istreambuf_iterator<char> (in)), {});
    const auto state = lynx::vfx::stateFromFlPreset (bytes, lynx::vfx::Kind::sequencer);
    CHECK (state.has_value());
    if (! state)
        return;
    const auto s = lynx::vfx::parseSequencer (*state);
    CHECK (s.has_value());
    if (! s)
        return;
    CHECK (lynx::vfx::writeSequencer (*s) == *state); // (byte for byte)
    CHECK (s->programs.size() == 128 && s->program() == 3);
    CHECK (s->current().nameText() == "Simple 4k Up 4 beats");
    using S = lynx::vfx::SequencerSettings;
    CHECK (s->get (S::steps) == 16 && s->get (S::gate) == 50 && s->get (S::missingKeys) == 3 && s->get (S::arpMode) == 1);
    // T1: C4 E4 G4 held → 60 64 67 67 (N4 missing: the last key) on every 1/16, velocity 96, half a step long
    auto p = lynx::vfx::makeProcessor (lynx::vfx::Kind::sequencer, *state);
    const auto notes = vfx164::sequence (*p, { { 0.0, 60, true }, { 0.0, 64, true }, { 0.0, 67, true } }, 1.0);
    CHECK (notes.size() >= 8);
    const int expected[8] = { 60, 64, 67, 67, 60, 64, 67, 67 };
    for (int i = 0; i < 8 && i < (int) notes.size(); ++i)
    {
        CHECK (notes[(std::size_t) i].key == expected[i]);
        CHECK (std::abs (notes[(std::size_t) i].start - 0.125 * i) < 1e-4);
        CHECK (notes[(std::size_t) i].velocity == 96);
        CHECK (std::abs (notes[(std::size_t) i].length - 0.0625) < 1e-3);
    }
    // T2: output velocity = the key's
    auto s2 = *s;
    s2.set (S::velocityMode, 1);
    auto p2 = vfx164::sequencer (s2);
    const auto loud = vfx164::sequence (*p2, { { 0.0, 60, true }, { 0.0, 64, true } }, 0.3);
    CHECK (! loud.empty() && loud[0].velocity == 100);
    // T3 / T4: missing keys cyclic / not played
    s2.set (S::velocityMode, 0);
    s2.set (S::missingKeys, 1);
    p2 = vfx164::sequencer (s2);
    auto two = vfx164::sequence (*p2, { { 0.0, 60, true }, { 0.0, 64, true } }, 0.49);
    CHECK (two.size() == 4 && two[2].key == 60 && two[3].key == 64);
    s2.set (S::missingKeys, 0);
    p2 = vfx164::sequencer (s2);
    two = vfx164::sequence (*p2, { { 0.0, 60, true }, { 0.0, 64, true } }, 0.99);
    CHECK (two.size() == 4 && two[2].key == 60 && std::abs (two[2].start - 0.5) < 1e-4);
}

TEST (vfx_sequencer_plays_fl_test_vectors)
{
    using namespace vfx164;
    using S = lynx::vfx::SequencerSettings;
    // T5: steps 0 = thru, transpose +2: the note passes at once and ends with its key
    auto s = blankProgram();
    s.set (S::steps, 0);
    s.set (S::transposeSemitones, 2);
    auto p = sequencer (s);
    auto notes = sequence (*p, { { 0.0, 60, true }, { 1.0, 60, false } }, 1.5);
    CHECK (notes.size() == 1 && notes[0].key == 62 && notes[0].start == 0.0 && std::abs (notes[0].length - 1.0) < 1e-3
           && notes[0].velocity == 100);
    // T6 / T7: C major forced, every scale step +1: D → E, C# snaps down to C → D; 0.9 of a step long
    s = blankProgram();
    s.set (S::forceKey, 2);
    s.set (S::forceScale, 2);
    for (int i = 0; i < 64; ++i)
        s.set (0x80 + i, 1);
    p = sequencer (s);
    notes = sequence (*p, { { 0.0, 62, true } }, 0.49);
    CHECK (notes.size() == 4 && notes[0].key == 64 && notes[3].key == 64 && std::abs (notes[0].length - 0.1125) < 1e-3);
    p = sequencer (s);
    notes = sequence (*p, { { 0.0, 61, true } }, 0.2);
    CHECK (! notes.empty() && notes[0].key == 62);
    // T8: swing +50: odd steps half a step late
    s = blankProgram();
    s.set (S::swing, 50);
    p = sequencer (s);
    notes = sequence (*p, { { 0.0, 60, true } }, 0.55);
    CHECK (notes.size() == 5);
    const double swung[5] = { 0.0, 0.1875, 0.25, 0.4375, 0.5 };
    for (int i = 0; i < 5 && i < (int) notes.size(); ++i)
        CHECK (std::abs (notes[(std::size_t) i].start - swung[i]) < 1e-3);
    // T9: output wrap 48…59: E4 plays E3
    s = blankProgram();
    s.set (S::outputLow, 48);
    s.set (S::outputHigh, 59);
    p = sequencer (s);
    notes = sequence (*p, { { 0.0, 64, true } }, 0.2);
    CHECK (! notes.empty() && notes[0].key == 52);
    // T10: a rest after a step lengthens it by a step; a tie makes it overlap the next (legato)
    s = blankProgram();
    s.set (0x100 + 1, S::rest);
    s.set (0x100 + 2, S::tie);
    p = sequencer (s);
    notes = sequence (*p, { { 0.0, 60, true } }, 0.3);
    CHECK (notes.size() == 2 && std::abs (notes[0].length - 1.9 * 0.125) < 1e-3 && std::abs (notes[1].start - 0.25) < 1e-4);
    s.set (0x100 + 1, S::tie);
    s.set (0x140 + 1, 0x08); // (N2: another key, so the tie overlaps it)
    p = sequencer (s);
    notes = sequence (*p, { { 0.0, 60, true }, { 0.0, 64, true } }, 0.2);
    CHECK (notes.size() == 2 && notes[1].key == 64 && std::abs (notes[0].length - 1.25 * 0.125) < 1e-3);
    // T11: chord normalized, every step a chord: G3 C4 E4 → C major in root position
    s = blankProgram();
    s.set (S::order, 6);
    for (int i = 0; i < 64; ++i)
        s.set (0x100 + i, S::chord);
    p = sequencer (s);
    notes = sequence (*p, { { 0.0, 55, true }, { 0.0, 60, true }, { 0.0, 64, true } }, 0.1);
    std::vector<int> keys;
    for (const auto& n : notes)
        keys.push_back (n.key);
    std::sort (keys.begin(), keys.end());
    CHECK ((keys == std::vector<int> { 60, 64, 67 }));
    // T12: stopped: the sequencer runs on its own clock while the key is held
    s = blankProgram();
    p = sequencer (s);
    notes = sequence (*p, { { 0.0, 60, true }, { 0.3, 60, false } }, 1.5, false);
    CHECK (notes.size() == 3 && std::abs (notes[2].start - 0.25) < 1e-3);
    CHECK (! p->busy());
    // octaves lane: bit 4 = +1 octave; key select: Fixed / Root
    s = blankProgram();
    s.set (0xC0, 0x10);
    s.set (0x140 + 1, 0x01);
    p = sequencer (s);
    notes = sequence (*p, { { 0.0, 60, true } }, 0.2);
    CHECK (notes.size() == 2 && notes[0].key == 72 && notes[1].key == 64);
    // Arp mode off: nothing; a stop releases what sounds
    s = blankProgram();
    s.set (S::arpMode, 0);
    p = sequencer (s);
    CHECK (sequence (*p, { { 0.0, 60, true } }, 0.3).empty());
}

TEST (vfx_sequencer_cost_per_block)
{
    // the audio-thread cost: 4 held keys, 16 steps of chords, 1 s of 128-sample blocks
    using namespace vfx164;
    using S = lynx::vfx::SequencerSettings;
    auto s = blankProgram();
    for (int i = 0; i < 64; ++i)
        s.set (0x100 + i, S::chord);
    auto p = sequencer (s);
    lynx::vfx::EventList out;
    lynx::vfx::Transport t;
    t.sampleRate = 48000.0;
    t.bpm = 140.0;
    t.playing = true;
    std::vector<lynx::vfx::Event> in { on (60), on (64), on (67), on (71) };
    const auto begin = std::chrono::steady_clock::now();
    int notes = 0;
    for (int b = 0; b < 375; ++b)
    {
        out.clear();
        t.ticks = b * 128.0 / 48000.0 * (140.0 / 60.0) * 960.0;
        p->process (b == 0 ? in.data() : nullptr, b == 0 ? 4 : 0, 128, t, out);
        for (int k = 0; k < out.size(); ++k)
            notes += out[k].type == lynx::vfx::Event::Type::on;
    }
    const auto ms = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - begin).count();
    std::printf ("  sequencer: 1 s of audio in %.3f ms (%d notes)\n", ms, notes);
    CHECK (notes > 30 && ms < 20.0);
}

TEST (vfx_script_keeps_fl_scripts_as_they_are)
{
    using namespace lynx::vfx;
    auto s = defaultScript();
    CHECK (s.source() == "import flvfx as vfx");
    s.setSource ("import flvfx as vfx\n\ndef onTriggerVoice(v):\n    v.trigger()\n");
    CHECK (s.text.find ("Source=import flvfx as vfx<linebreak><linebreak>def onTriggerVoice(v):<linebreak>") != std::string::npos);
    CHECK (s.source() == "import flvfx as vfx\n\ndef onTriggerVoice(v):\n    v.trigger()\n");
    CHECK (parseScript (writeScript (s)) == s);
    CHECK (! parseScript ({ 9, 0, 0, 0, 'x' }));
    // a script plays its notes through unchanged
    auto p = makeProcessor (Kind::script, writeScript (s));
    const auto out = vfx164::run (*p, { vfx164::on (60) });
    CHECK (out.size() == 1 && out[0].voice.key == 60);
    if (realFlpDir.empty())
        return;
    namespace fs = std::filesystem;
    const auto dir = fs::path (realFlpDir) / "Patches" / "Plugin presets" / "Effects" / "VFX Script";
    if (! fs::is_directory (dir))
        return;
    int files = 0;
    for (const auto& entry : fs::recursive_directory_iterator (dir))
    {
        if (entry.path().extension() != ".fst")
            continue;
        std::ifstream in (entry.path(), std::ios::binary);
        const std::vector<std::uint8_t> bytes ((std::istreambuf_iterator<char> (in)), {});
        const auto state = stateFromFlPreset (bytes, Kind::script);
        CHECK (state.has_value());
        if (! state)
            continue;
        const auto script = parseScript (*state);
        CHECK (script && writeScript (*script) == *state);
        if (! script)
            continue;
        ++files;
        // the source written back as it was read changes nothing
        auto again = *script;
        again.setSource (script->source());
        CHECK (again.text == script->text);
        if (entry.path().filename() == "Arpeggiator.fst")
        {
            const auto params = script->parameters();
            CHECK (std::find (params.begin(), params.end(), std::pair<std::string, std::string> { "Speed", "2" }) != params.end());
            CHECK (std::find (params.begin(), params.end(), std::pair<std::string, std::string> { "Duration", "196608" }) != params.end());
            CHECK (script->source().find ("<linebreak>") == std::string::npos && script->source().size() > 1000);
        }
    }
    std::printf ("  %d FL scripts\n", files);
    CHECK (files >= 80);
}
