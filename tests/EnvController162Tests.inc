// Round 162 (issue #7): the Envelope Controller's LFO, keyboard / velocity / Mod X / Mod Y / random maps, ADSR knobs,
// global Mod X / Y and continuous output (docs/analise/REA_rodada162e-envelope.md).

namespace ec162
{
// the 0xD5 state of one of the installation's Envelope Controller presets (empty without an installation)
std::vector<std::uint8_t> presetState (const char* relative)
{
    if (realFlpDir.empty())
        return {};
    std::ifstream in (std::filesystem::path (realFlpDir) / "Patches" / "Plugin presets" / "Generators" / "Fruity Envelope Controller" / relative,
                      std::ios::binary);
    const std::vector<std::uint8_t> b ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());
    std::size_t i = 0;
    while (i + 8 <= b.size() && std::memcmp (b.data() + i, "FLdt", 4) != 0)
        ++i;
    i += 8;
    while (i < b.size())
    {
        const auto id = b[i++];
        std::size_t n = id < 64 ? 1 : id < 128 ? 2 : id < 192 ? 4 : 0;
        if (id >= 192)
            for (int shift = 0; i < b.size(); shift += 7)
            {
                const auto c = b[i++];
                n |= (std::size_t) (c & 0x7F) << shift;
                if (c < 0x80)
                    break;
            }
        if (id == 0xD5 && i + n <= b.size())
            return { b.begin() + (std::ptrdiff_t) i, b.begin() + (std::ptrdiff_t) (i + n) };
        i += n;
    }
    return {};
}

lynx::ControllerCurve line (float y0, float y1)
{
    lynx::ControllerCurve c;
    c.count = 2;
    c.time = { 0.0f, 1.0f };
    c.level = { y0, y1 };
    return c;
}

bool near (double a, double b, double tolerance = 1.0e-4) { return std::abs (a - b) <= tolerance; }
} // namespace ec162

TEST (envelope_controller_velocity_input_and_lfo_rate_follow_fl)
{
    // FL's velocity map input undoes its velocity -> volume law: ≈ 0.9918 · velocity
    CHECK (ec162::near (lynx::controllerVelocityInput (0.0f), 0.0));
    CHECK (ec162::near (lynx::controllerVelocityInput (1.0f), 0.9918, 2.0e-3));
    CHECK (ec162::near (lynx::controllerVelocityInput (100.0f / 128.0f), 0.775, 2.0e-3));
    CHECK (lynx::controllerVelocityInput (0.3f) < lynx::controllerVelocityInput (0.6f));

    // LFO speed: period 625 / (10001^(v/65536) − 1)
    lynx::ControllerArticulator a;
    a.lfoSpeed = 40898;
    CHECK (ec162::near (1.0 / lynx::controllerLfoRate (a), 2.0, 2.0e-3));
    a.lfoSpeed = 65536;
    CHECK (ec162::near (1.0 / lynx::controllerLfoRate (a), 0.0625, 1.0e-4));
}

TEST (envelope_controller_lfo_wave_shapes)
{
    lynx::ControllerArticulator a; // straight triangle: 1 → −1 over the first half, back to 1
    CHECK (ec162::near (lynx::controllerLfoWave (a, 0.0), 1.0));
    CHECK (ec162::near (lynx::controllerLfoWave (a, 0.25), 0.0));
    CHECK (ec162::near (lynx::controllerLfoWave (a, 0.5), -1.0, 1.0e-3));
    CHECK (ec162::near (lynx::controllerLfoWave (a, 0.75), 0.0));
    CHECK (ec162::near (lynx::controllerLfoWave (a, 1.25), 0.0)); // periodic
    // a note starts the LFO at 0, rising
    const auto start = lynx::controllerLfoStartPhase (a);
    CHECK (ec162::near (lynx::controllerLfoWave (a, start), 0.0, 1.0e-3));
    CHECK (lynx::controllerLfoWave (a, start + 0.05) > 0.1f);

    // skew moves the bottom of the wave; pulse width > 0 holds the top at the end of the cycle
    a.lfoSkew = 64; // skew 0.25: the bottom comes at a quarter of the cycle instead of half
    CHECK (ec162::near (lynx::controllerLfoWave (a, 0.25), -1.0, 1.0e-3));
    CHECK (ec162::near (lynx::controllerLfoWave (a, 0.125), 0.0, 1.0e-3));
    CHECK (lynx::controllerLfoWave (a, 0.5) > -0.5f);
    a.lfoSkew = 0;
    a.lfoPulseWidth = 64;
    CHECK (ec162::near (lynx::controllerLfoWave (a, 0.9), 1.0));
    CHECK (ec162::near (lynx::controllerLfoWave (a, 0.25), -1.0, 1.0e-3)); // the triangle runs in the first half
    a.lfoPulseWidth = 0;

    // tension bends the triangle: positive pushes the values outwards, negative inwards (sign kept)
    a.lfoTension = 64;
    const auto bent = lynx::controllerLfoWave (a, 0.125); // straight: 0.5
    CHECK (bent > 0.6f && bent < 1.0f);
    a.lfoTension = -64;
    const auto pulled = lynx::controllerLfoWave (a, 0.125);
    CHECK (pulled > 0.0f && pulled < 0.4f);
    CHECK (ec162::near (lynx::controllerLfoWave (a, 0.375), -pulled, 1.0e-4));
}

TEST (envelope_controller_maps_and_modes)
{
    // bipolar: base + (map − 0.5); unipolar: base × map
    lynx::ControllerArticulator a;
    a.bipolar = true;
    a.base = 0.5f;
    a.maps[lynx::mapModX] = ec162::line (0.0f, 1.0f);
    CHECK (ec162::near (lynx::controllerRestingValue (a, 0.2f, 0.5f), 0.2));
    CHECK (ec162::near (lynx::controllerRestingValue (a, 0.9f, 0.5f), 0.9));
    a.bipolar = false;
    a.base = 1.0f;
    a.maps[lynx::mapModX] = ec162::line (1.0f, 0.0f); // FL's "Smart X … (xfade)"
    a.maps[lynx::mapModY] = ec162::line (0.0f, 1.0f);
    CHECK (ec162::near (lynx::controllerRestingValue (a, 0.25f, 0.5f), 0.75 * 0.5));
    // empty maps change nothing in either mode
    lynx::ControllerArticulator plain;
    plain.base = 0.3f;
    CHECK (ec162::near (lynx::controllerRestingValue (plain, 0.0f, 1.0f), 0.3));
    plain.bipolar = true;
    CHECK (ec162::near (lynx::controllerRestingValue (plain, 0.0f, 1.0f), 0.3));

    // keyboard map: a step at key 24 (x = key / 120), unipolar gate envelope
    lynx::ControllerArticulator gate;
    gate.base = 0.0f;
    gate.envOn = true;
    gate.count = 2;
    gate.time = { 0.0f, 0.2f };
    gate.level = { 1.0f, 1.0f };
    auto& key = gate.maps[lynx::mapKeyboard];
    key.count = 4;
    key.time = { 0.0f, 0.1917f, 0.2f, 0.2083f };
    key.level = { 0.0f, 0.0f, 1.0f, 0.0f };
    lynx::ControllerNote note;
    note.key = 24.0f;
    CHECK (ec162::near (lynx::controllerValue (gate, 0.1, -1.0, 0.0, note, 0.5f, 0.5f), 1.0, 1.0e-3));
    note.key = 26.0f;
    CHECK (ec162::near (lynx::controllerValue (gate, 0.1, -1.0, 0.0, note, 0.5f, 0.5f), 0.0, 1.0e-3));

    // velocity map (unipolar, FL's "Note gate (velocity)") and the random map
    gate.maps[lynx::mapKeyboard] = {};
    gate.maps[lynx::mapVelocity] = ec162::line (0.0f, 1.0f);
    note.velocity = 0.5f;
    CHECK (ec162::near (lynx::controllerValue (gate, 0.1, -1.0, 0.0, note, 0.5f, 0.5f), lynx::controllerVelocityInput (0.5f), 1.0e-3));
    gate.maps[lynx::mapRandom] = ec162::line (0.0f, 1.0f);
    note.random = 0.5f;
    CHECK (ec162::near (lynx::controllerValue (gate, 0.1, -1.0, 0.0, note, 0.5f, 0.5f), 0.5 * lynx::controllerVelocityInput (0.5f), 1.0e-3));

    // the note's own Mod X is added to the global knob (0.5 = none)
    lynx::ControllerArticulator xs;
    xs.bipolar = true;
    xs.base = 0.5f;
    xs.maps[lynx::mapModX] = ec162::line (0.0f, 1.0f);
    note = {};
    note.modX = 0.7f;
    CHECK (ec162::near (lynx::controllerValue (xs, 0.0, -1.0, 0.0, note, 0.4f, 0.5f), 0.6, 1.0e-3));
}

TEST (envelope_controller_lfo_combines_by_mode)
{
    lynx::ControllerArticulator a;
    a.lfoOn = true;
    a.lfoLevel = 0.5f;
    a.base = 0.5f;
    a.bipolar = true;
    lynx::ControllerNote note;
    // phase 0: wave = 1 → bipolar adds 0.5
    CHECK (ec162::near (lynx::controllerValue (a, 0.0, -1.0, 0.0, note, 0.5f, 0.5f), 1.0));
    CHECK (ec162::near (lynx::controllerValue (a, 0.0, -1.0, 0.5, note, 0.5f, 0.5f), 0.0, 1.0e-3));
    // unipolar multiplies by 1 + 2 · wave · level
    a.bipolar = false;
    a.base = 0.4f;
    CHECK (ec162::near (lynx::controllerValue (a, 0.0, -1.0, 0.0, note, 0.5f, 0.5f), 0.8));
    CHECK (ec162::near (lynx::controllerValue (a, 0.0, -1.0, 0.25, note, 0.5f, 0.5f), 0.4));
    // the amplitude curve fades it in
    a.lfoAmp.count = 2;
    a.lfoAmp.time = { 0.0f, 1.0f };
    a.lfoAmp.level = { 0.0f, 1.0f };
    CHECK (ec162::near (lynx::controllerValue (a, 0.0, -1.0, 0.0, note, 0.5f, 0.5f), 0.4));
    CHECK (ec162::near (lynx::controllerValue (a, 0.5, -1.0, 0.0, note, 0.5f, 0.5f), 0.6));
    // a voice ends once envelope and LFO amplitude have run out after the release
    CHECK (! lynx::controllerVoiceFinished (a, 0.5, -1.0));
    CHECK (lynx::controllerVoiceFinished (a, 2.0, 0.1));
}

TEST (envelope_controller_adsr_knobs)
{
    auto a = lynx::defaultEnvelopeController()[0]; // 0 → 1 (0.02 s) → 0.6 sustain (0.3 s) → 0 (0.8 s)
    CHECK (lynx::controllerWithKnobs (a) == a);
    a.adsr = { 256, 64, 0, 128 };
    const auto k = lynx::controllerWithKnobs (a);
    CHECK (ec162::near (k.time[1], 0.04) && ec162::near (k.time[2], 0.04 + 0.28 * 0.5) && ec162::near (k.time[3], k.time[2] + 0.5));
    a.adsr = { 128, 128, 26, 128 };
    const auto s = lynx::controllerWithKnobs (a);
    CHECK (ec162::near (s.level[2], 0.6 + 26.0 / 128.0) && ec162::near (s.level[0], 0.0) && ec162::near (s.level[3], 0.0));
}

TEST (envelope_controller_lyec_versions_round_trip)
{
    // version 1 (envelopes only) still loads
    std::vector<std::uint8_t> v1;
    auto put = [&] (const void* p, std::size_t n) { v1.insert (v1.end(), (const std::uint8_t*) p, (const std::uint8_t*) p + n); };
    const std::int32_t header[2] { 0x4345594C, 1 };
    put (header, 8);
    for (int k = 0; k < 8; ++k)
    {
        const std::int32_t flags = 1 | 2;
        const float base = 0.25f, level = 0.5f;
        const std::int32_t ints[4] { -1, 10, 256, 2 };
        const float points[6] { 0.0f, 0.0f, 0.0f, 0.5f, 1.0f, 0.3f };
        put (&flags, 4);
        put (&base, 4);
        put (&level, 4);
        put (ints, 16);
        put (points, 24);
    }
    const auto old = lynx::parseEnvelopeController (v1);
    CHECK (old.has_value());
    if (old)
    {
        CHECK ((*old)[3].bipolar && (*old)[3].envOn && (*old)[3].lowKey == 10 && (*old)[3].count == 2 && ec162::near ((*old)[3].tension[1], 0.3));
        CHECK (! (*old)[3].lfoOn && (*old)[3].maps[0].count == 0 && ! old->continuous && ec162::near (old->modX, 0.5));
    }

    // version 2 keeps everything
    auto d = lynx::defaultEnvelopeController();
    d.modX = 0.3f;
    d.modY = 0.8f;
    d.continuous = true;
    d[2].lfoOn = d[2].lfoTempo = d[2].lfoGlobal = true;
    d[2].lfoSpeed = 12345;
    d[2].lfoTension = -40;
    d[2].lfoSkew = 20;
    d[2].lfoPulseWidth = 100;
    d[2].lfoLevel = -0.25f;
    d[2].lfoAmp = ec162::line (0.0f, 1.0f);
    d[2].lfoAmp.sustain = 1;
    d[2].adsr = { 100, 200, -30, 50 };
    d[5].maps[lynx::mapRandom] = ec162::line (0.2f, 0.9f);
    d[5].maps[lynx::mapModY].tension[0] = 0.5f;
    d[5].maps[lynx::mapModY].count = 2;
    d[5].maps[lynx::mapModY].time = { 0.1f, 0.6f };
    const auto back = lynx::parseEnvelopeController (lynx::writeEnvelopeController (d));
    CHECK (back.has_value());
    if (back)
    {
        CHECK (ec162::near (back->modX, 0.3) && ec162::near (back->modY, 0.8) && back->continuous);
        for (std::size_t k = 0; k < 8; ++k)
            CHECK ((*back)[k] == d[k]);
    }
}

TEST (envelope_controller_fl_presets_import_lfo_and_maps)
{
    if (realFlpDir.empty())
    {
        std::printf ("  (no FL Studio data: skipped)\n");
        return;
    }
    // every preset loads
    int presets = 0, read = 0;
    const auto dir = std::filesystem::path (realFlpDir) / "Patches" / "Plugin presets" / "Generators" / "Fruity Envelope Controller";
    for (auto& entry : std::filesystem::recursive_directory_iterator (dir))
        if (entry.path().extension() == ".fst")
        {
            ++presets;
            const auto relative = std::filesystem::relative (entry.path(), dir).string();
            read += lynx::parseEnvelopeController (ec162::presetState (relative.c_str())).has_value();
        }
    std::printf ("  %d Envelope Controller presets, %d read\n", presets, read);
    CHECK (presets >= 20 && read == presets);

    // "Smart X for 2 controls (inverted)": continuous, bipolar, Mod X maps 0→1 and 1→0
    if (const auto smart = lynx::parseEnvelopeController (ec162::presetState ("Smart controls/Smart X for 2 controls (inverted).fst")))
    {
        CHECK (smart->continuous && ec162::near (smart->modX, 0.5));
        const auto& a = (*smart)[0];
        CHECK (a.bipolar && a.maps[lynx::mapModX].count == 2 && a.maps[lynx::mapModY].count == 0 && a.maps[lynx::mapKeyboard].count == 0);
        CHECK (ec162::near (lynx::controllerRestingValue (a, 0.25f, 0.5f), 0.25, 1.0e-3));
        CHECK (ec162::near (lynx::controllerRestingValue ((*smart)[1], 0.25f, 0.5f), 0.75, 1.0e-3));
    }
    else
        CHECK (false);

    // "Note LFO": a tempo-synced global LFO at 2 beats
    if (const auto lfo = lynx::parseEnvelopeController (ec162::presetState ("Misc/Note LFO.fst")))
    {
        const auto& a = (*lfo)[0];
        CHECK (a.lfoOn && a.lfoTempo && a.lfoGlobal && a.lfoSpeed == 40898 && a.lfoAmp.count == 0 && ec162::near (a.lfoLevel, 0.5));
        CHECK (! (*lfo)[1].lfoOn);
    }
    else
        CHECK (false);

    // "Note gate (velocity)": unipolar, velocity map 0→1
    if (const auto vel = lynx::parseEnvelopeController (ec162::presetState ("Misc/Note gate (velocity).fst")))
    {
        const auto& a = (*vel)[0];
        CHECK (! a.bipolar && a.maps[lynx::mapVelocity].count == 2 && ec162::near (a.maps[lynx::mapVelocity].level[1], 1.0));
        lynx::ControllerNote note;
        note.velocity = 0.25f;
        CHECK (ec162::near (lynx::controllerValue (a, 0.1, -1.0, 0.0, note, 0.5f, 0.5f), lynx::controllerVelocityInput (0.25f), 1.0e-3));
    }
    else
        CHECK (false);

    // "Note switches (C2-G2 mono)": output k is 1 only on key 24 + k
    if (const auto sw = lynx::parseEnvelopeController (ec162::presetState ("Misc/Note switches (C2-G2 mono).fst")))
    {
        lynx::ControllerNote note;
        for (int k : { 0, 3, 7 })
        {
            note.key = 24.0f + (float) k;
            CHECK (ec162::near (lynx::controllerValue ((*sw)[(std::size_t) k], 0.0, -1.0, 0.0, note, 0.5f, 0.5f), 1.0, 1.0e-3));
            note.key = 24.0f + (float) k + 1.0f;
            CHECK (ec162::near (lynx::controllerValue ((*sw)[(std::size_t) k], 0.0, -1.0, 0.0, note, 0.5f, 0.5f), 0.0, 1.0e-3));
        }
    }
    else
        CHECK (false);

    // the unipolar default keeps its maps neutral (empty)
    if (const auto product = lynx::parseEnvelopeController (ec162::presetState ("Default (unipolar product).fst")))
        for (const auto& m : (*product)[0].maps)
            CHECK (m.count == 0);
    else
        CHECK (false);
}

TEST (envelope_controller_instrument_plays_maps_and_continuous_output)
{
    auto d = lynx::defaultEnvelopeController();
    d.continuous = true;
    d.modX = 0.2f;
    d[0].bipolar = true;
    d[0].envOn = false;
    d[0].base = 0.5f;
    d[0].maps[lynx::mapModX] = ec162::line (0.0f, 1.0f);
    // output 1: velocity map, unipolar gate
    d[1].envOn = true;
    d[1].base = 0.0f;
    d[1].count = 2;
    d[1].time = { 0.0f, 1.0f };
    d[1].level = { 1.0f, 1.0f };
    d[1].sustain = 1;
    d[1].maps[lynx::mapVelocity] = ec162::line (0.0f, 1.0f);
    lynx::ChannelDef c;
    c.instrument = lynx::InstrumentKind::controller;
    c.controllerState = lynx::writeEnvelopeController (d);
    auto inst = lynx::makeInstrument (lynx::InstrumentKind::controller);
    inst->prepare (48000.0);
    inst->configure (c, 120.0);
    std::vector<float> l (480), r (480);
    inst->render (l.data(), r.data(), 480, 1.0f, 1.0f);
    CHECK (ec162::near (inst->controlOutput (0), 0.2, 1.0e-3)); // no note: follows Mod X
    inst->setNextNoteModulation (0.9f, 0.5f);
    inst->noteOn (60, 0.5f);
    inst->render (l.data(), r.data(), 480, 1.0f, 1.0f);
    CHECK (ec162::near (inst->controlOutput (0), 0.6, 1.0e-3)); // the note adds its own Mod X (+0.4)
    CHECK (ec162::near (inst->controlOutput (1), lynx::controllerVelocityInput (0.5f), 1.0e-3));
}
