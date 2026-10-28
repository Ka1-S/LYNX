// Round 165c (vfxfix165): the VFX leftovers of 0.1.19 — Level Scaler on held notes, Envelope loop points, Keyboard
// Splitter's zone-1 fallback and Manual mapping, Sequencer program changes and chains (core/Vfx.h)

namespace vfx165c
{
// the voice's velocity after each block of `n` samples (the last prop seen, else the note-on's)
std::vector<float> velocities (lynx::vfx::Processor& p, int blocks, int n, lynx::vfx::Transport t, std::vector<lynx::vfx::Event> first)
{
    std::vector<float> out;
    float v = -1.0f;
    for (int b = 0; b < blocks; ++b)
    {
        lynx::vfx::EventList o;
        if (b == 0)
            p.process (first.data(), (int) first.size(), n, t, o);
        else
            p.process (nullptr, 0, n, t, o);
        for (int k = 0; k < o.size(); ++k)
        {
            if (o[k].type == lynx::vfx::Event::Type::on)
                v = o[k].voice.velocity;
            else if (o[k].type == lynx::vfx::Event::Type::prop && o[k].prop == lynx::vfx::Prop::velocity)
                v = o[k].value;
        }
        out.push_back (v);
    }
    return out;
}
} // namespace vfx165c

TEST (vfx165c_level_scaler_moves_held_notes)
{
    using namespace vfx164;
    using L = lynx::vfx::LevelScalerSettings;
    auto p = lynx::vfx::makeProcessor (lynx::vfx::Kind::levelScaler, lynx::vfx::writeLevelScaler (lynx::vfx::defaultLevelScaler()));
    auto out = run (*p, { on (60, 0, 0, 0.9f) });
    CHECK (out.size() == 1 && std::abs (out[0].voice.velocity - 0.9f) < 1e-5f);
    const auto id = out.empty() ? 0 : out[0].id;
    CHECK (p->busy());
    // an automation of "Velocity - Multiplier" (0.5 → 0) and "Velocity - Offset" (0.75 → +4096): the held note follows
    p->setParameter (L::velocity * 6 + L::multiply, 0.5f);
    p->setParameter (L::velocity * 6 + L::offset, 0.75f);
    out = run (*p, {});
    CHECK (out.size() == 1 && out[0].type == lynx::vfx::Event::Type::prop && out[0].prop == lynx::vfx::Prop::velocity
           && out[0].id == id && std::abs (out[0].value - 0.5f) < 1e-4f);
    CHECK (run (*p, {}).empty()); // (nothing moved: nothing sent)
    // a VFX before it moves the voice's pan: the pan curve (identity) passes it on
    lynx::vfx::Event pan;
    pan.type = lynx::vfx::Event::Type::prop;
    pan.id = lynx::vfx::inputId (60, 0);
    pan.prop = lynx::vfx::Prop::pan;
    pan.value = -0.5f;
    out = run (*p, { pan });
    CHECK (out.size() == 1 && out[0].prop == lynx::vfx::Prop::pan && std::abs (out[0].value + 0.5f) < 1e-5f);
    // the release goes out on the voice it sent
    out = run (*p, { off (60) });
    CHECK (out.size() == 1 && out[0].type == lynx::vfx::Event::Type::off && out[0].id == id);
    CHECK (! p->busy());
    // a new state (the editor) also moves the held notes
    p = lynx::vfx::makeProcessor (lynx::vfx::Kind::levelScaler, lynx::vfx::writeLevelScaler (lynx::vfx::defaultLevelScaler()));
    run (*p, { on (62, 0, 0, 0.4f) });
    auto fixed = lynx::vfx::defaultLevelScaler();
    fixed.at (L::velocity, L::multiply) = 0;
    fixed.at (L::velocity, L::offset) = 2048;
    CHECK (p->configure (lynx::vfx::writeLevelScaler (fixed)));
    out = run (*p, {});
    CHECK (out.size() == 1 && out[0].prop == lynx::vfx::Prop::velocity && std::abs (out[0].value - 0.25f) < 1e-4f);
}

TEST (vfx165c_level_scaler_shows_its_events)
{
    using namespace vfx164;
    using L = lynx::vfx::LevelScalerSettings;
    auto s = lynx::vfx::defaultLevelScaler();
    s.at (L::velocity, L::multiply) = 4096; // half the slope
    auto p = lynx::vfx::makeProcessor (lynx::vfx::Kind::levelScaler, lynx::vfx::writeLevelScaler (s));
    lynx::vfx::Monitor::Item items[16];
    run (*p, { on (60, 0, 0, 0.8f) });
    CHECK (p->monitor().pop (items, 16) == 0); // (no editor watching: nothing kept)
    p->monitor().watching = true;
    run (*p, { on (64, 0, 0, 0.8f) });
    const auto n = p->monitor().pop (items, 16);
    CHECK (n == 5); // pan, Mod X, Mod Y, fine pitch, velocity
    bool velocity = false;
    for (int k = 0; k < n; ++k)
        if (items[k].kind == L::velocity)
            velocity = std::abs (items[k].in - 0.8f) < 1e-5f && std::abs (items[k].out - 0.4f) < 1e-4f;
    CHECK (velocity);
    run (*p, { off (64) });
    CHECK (p->monitor().pop (items, 16) == 1 && items[0].kind == L::release);
}

TEST (vfx165c_envelope_loops_between_its_loop_and_sustain_points)
{
    using namespace vfx164;
    using E = lynx::vfx::EnvelopeSettings;
    // FL's "Pumping": a tempo envelope (0,0) → (½ beat, 1) → (1 beat, 0 as a step), loop start 0, sustain 2
    auto s = lynx::vfx::defaultEnvelope();
    auto& part = s.articulators[0].parts[E::velocity];
    part.envelope.points.clear();
    lynx::vfx::MapCurve::Point a, b, c;
    b.x = 0.5;
    b.y = 1.0;
    c.x = 1.0;
    c.curve = 2;
    part.envelope.points = { a, b, c };
    part.envelope.footer = { 1, 1, 0, 0, 2 };
    lynx::vfx::Transport t;
    t.sampleRate = 48000.0;
    t.bpm = 120.0; // 1 beat = 24000 samples
    auto p = lynx::vfx::makeProcessor (lynx::vfx::Kind::envelope, lynx::vfx::writeEnvelope (s));
    const auto v = vfx165c::velocities (*p, 120, 600, t, { on (60, 0, 0, 0.78125f) }); // 3 beats in 1/40 beat blocks
    // a quarter beat in and a beat later: the same level (it loops); just before each beat the top, just after it the ramp from 0
    CHECK (std::abs (v[10] - v[50]) < 0.02f && std::abs (v[10] - v[90]) < 0.02f && v[10] > 0.2f);
    CHECK (v[35] > 0.7f && v[75] > 0.7f && v[115] > 0.7f);
    CHECK (v[41] < 0.3f && v[81] < 0.3f && v[41] < v[39] - 0.5f); // (the step back to 0, then the ramp again)
    // the release: from the sustain point on (its level 0 here), the loop ends
    lynx::vfx::EventList o;
    auto offs = std::vector<lynx::vfx::Event> { off (60) };
    p->process (offs.data(), 1, 600, t, o);
    bool released = false;
    for (int k = 0; k < 400 && p->busy(); ++k)
    {
        o.clear();
        p->process (nullptr, 0, 600, t, o);
        for (int e = 0; e < o.size(); ++e)
            released = released || o[e].type == lynx::vfx::Event::Type::off;
    }
    CHECK (released || ! p->busy());
    // FL's own preset, when FL Studio is installed
    if (! realFlpDir.empty())
    {
        const auto file = std::filesystem::path (realFlpDir) / "Patches" / "Plugin presets" / "Effects" / "VFX Envelope" / "Pumping.fst";
        std::ifstream in (file, std::ios::binary);
        const std::vector<std::uint8_t> bytes ((std::istreambuf_iterator<char> (in)), {});
        if (const auto state = lynx::vfx::stateFromFlPreset (bytes, lynx::vfx::Kind::envelope))
        {
            p = lynx::vfx::makeProcessor (lynx::vfx::Kind::envelope, *state);
            const auto w = vfx165c::velocities (*p, 120, 600, t, { on (60, 0, 0, 0.78125f) });
            CHECK (std::abs (w[10] - w[50]) < 0.02f && std::abs (w[20] - w[100]) < 0.02f);
            CHECK (*std::max_element (w.begin() + 80, w.end()) > 0.5f); // (still pumping in the third beat)
        }
        else
            CHECK (false);
    }
}

TEST (vfx165c_keyboard_splitter_falls_back_on_zone_1)
{
    using namespace vfx164;
    using S = lynx::vfx::KeyboardSplitterSettings;
    // zone 1: an ordinary zone for the low keys; zone 2: random, on the high keys only
    S s;
    s.zones[0].keyboard.points = { { 0.0, 1.0 }, { 59.0 / 132.0, 1.0 }, { 60.0 / 132.0, 0.0 } };
    s.zones[1].keyboard.points = { { 71.0 / 132.0, 0.0 }, { 72.0 / 132.0, 1.0 } };
    s.zones[1].random = true;
    auto p = lynx::vfx::makeProcessor (lynx::vfx::Kind::keyboardSplitter, lynx::vfx::writeKeyboardSplitter (s));
    // key 80: zone 2 wins; key 64: no random zone has it, FL plays zone 1 (at full level, though its map is 0 there)
    auto out = run (*p, { on (80) });
    CHECK (out.size() == 1 && out[0].output == 1);
    out = run (*p, { on (64, 0, 0, 0.5f) });
    CHECK (out.size() == 1 && out[0].output == 0 && std::abs (out[0].voice.velocity - 0.5f) < 1e-4f);
    // zone 1 off: nothing
    s.zones[0] = {};
    p = lynx::vfx::makeProcessor (lynx::vfx::Kind::keyboardSplitter, lynx::vfx::writeKeyboardSplitter (s));
    CHECK (run (*p, { on (64) }).empty());
}

TEST (vfx165c_keyboard_splitter_manual_mapping)
{
    using namespace vfx164;
    // FL's test: from Default, press 48, 60, 72 and release → zone 1 = 0…47, 2 = 48…59, 3 = 60…71, 4 = 72…131
    auto s = lynx::vfx::defaultKeyboardSplitter();
    lynx::vfx::SplitterMapping m;
    lynx::vfx::beginManualMapping (m);
    CHECK (lynx::vfx::manualMapKey (s, m, 48) && lynx::vfx::manualMapKey (s, m, 60));
    CHECK (! lynx::vfx::manualMapKey (s, m, 55)); // (keys must rise)
    CHECK (lynx::vfx::manualMapKey (s, m, 72));
    lynx::vfx::endManualMapping (s, m);
    CHECK (! m.on);
    auto zoneOf = [&] (int key) {
        int found = -1, count = 0;
        for (int z = 0; z < 16; ++z)
            if (s.zones[(std::size_t) z].keyboard.at (key / 132.0) > 0.999)
            {
                found = z;
                ++count;
            }
        return count == 1 ? found : -1;
    };
    CHECK (zoneOf (0) == 0 && zoneOf (47) == 0 && zoneOf (48) == 1 && zoneOf (59) == 1 && zoneOf (60) == 2 && zoneOf (71) == 2);
    CHECK (zoneOf (72) == 3 && zoneOf (131) == 3 && zoneOf (127) == 3);
    CHECK (! s.zones[4].active());
    // the processor plays the new map; while an editor maps, the notes are taken (and shown), not played
    auto p = lynx::vfx::makeProcessor (lynx::vfx::Kind::keyboardSplitter, lynx::vfx::writeKeyboardSplitter (s));
    auto out = run (*p, { on (50), on (100) });
    CHECK (out.size() == 2 && out[0].output == 1 && out[1].output == 3);
    p->monitor().mapping = true;
    CHECK (run (*p, { on (40) }).empty());
    lynx::vfx::Monitor::Item items[4];
    CHECK (p->monitor().pop (items, 4) == 1 && items[0].in == 40.0f);
    // fifteen keys: sixteen zones, the mapping ends by itself
    s = lynx::vfx::defaultKeyboardSplitter();
    lynx::vfx::beginManualMapping (m);
    for (int k = 0; k < 15; ++k)
        lynx::vfx::manualMapKey (s, m, 40 + 3 * k);
    CHECK (! m.on && s.zones[15].active() && s.zones[15].keyboard.at (1.0) > 0.999 && s.zones[15].keyboard.at (80.0 / 132.0) < 1e-9);
}
