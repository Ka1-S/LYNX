// Round 164e (misc164m): the mod wheel and aftertouch on native instruments (docs/analise/REA_rodada164e-modwheel.md)

namespace misc164m
{
// records the live controllers it receives
struct Spy final : lynx::Instrument
{
    int messages = 0, lastStatus = 0, lastData1 = -1, controls = 0, lastCc = -1;
    float bend = 0.0f, lastValue = -1.0f;
    void prepare (double) noexcept override {}
    void noteOn (int, float) noexcept override {}
    void noteOff (int) noexcept override {}
    void allNotesOff() noexcept override {}
    void render (float*, float*, int, float, float) noexcept override {}
    void midiMessage (const std::uint8_t* bytes, int size) noexcept override
    {
        ++messages;
        lastStatus = size > 0 ? bytes[0] : 0;
        lastData1 = size > 1 ? bytes[1] : -1;
    }
    void pitchBend (float semitones) noexcept override { bend = semitones; }
    void controlChange (int cc, float value) noexcept override
    {
        ++controls;
        lastCc = cc;
        lastValue = value;
    }
};
} // namespace misc164m

TEST (misc164m_patcher_passes_live_controllers_to_its_instruments)
{
    // FL's Patcher takes MIDI input (its TFruityPlug MIDIIn is implemented, unlike FL's native generators) and passes
    // it to the plugins cabled to "From FL Studio": the mod wheel / CCs / pressure / bend reach a plugin inside it
    using namespace patcher162;
    lynx::ChannelDef channel;
    channel.instrument = lynx::InstrumentKind::patcher;
    channel.patcher.nodes = { generatorNode (lynx::InstrumentKind::synth), generatorNode (lynx::InstrumentKind::synth) };
    channel.patcher.wires = { midi (lynx::patchFromHost, 0, 0, 0), patcher141::audio (0, lynx::patchToHost), patcher141::audio (1, lynx::patchToHost) };
    std::vector<std::shared_ptr<misc164m::Spy>> spies;
    auto f = lynx::patcher::builtInFactories (48000.0);
    f.instrument = [&spies] (const lynx::ChannelDef&) {
        spies.push_back (std::make_shared<misc164m::Spy>());
        return std::shared_ptr<lynx::Instrument> (spies.back());
    };
    auto i = lynx::patcher::makeInstrument (channel, 48000.0, f);
    CHECK (spies.size() == 2u);
    if (spies.size() != 2u)
        return;
    const std::uint8_t wheel[3] { 0xB0, 1, 100 }, pressure[2] { 0xD0, 90 };
    i->midiMessage (wheel, 3);
    i->controlChange (1, 100.0f / 127.0f);
    i->midiMessage (pressure, 2);
    i->pitchBend (-1.5f);
    const auto& cabled = *spies[0];
    CHECK (cabled.messages == 2 && cabled.lastStatus == 0xD0 && cabled.lastData1 == 90);
    CHECK (cabled.controls == 1 && cabled.lastCc == 1 && std::abs (cabled.lastValue - 100.0f / 127.0f) < 1e-6f);
    CHECK (cabled.bend == -1.5f);
    // a generator not cabled to the notes gets none of it, like its notes
    CHECK (spies[1]->messages == 0 && spies[1]->controls == 0 && spies[1]->bend == 0.0f);
}

TEST (misc164m_native_engines_keep_ignoring_the_mod_wheel_like_fl)
{
    // [EXEC] FL's native generators never ask for MIDI input (no FHD_WantMIDIInput at creation; PoiZone, Sakura, 3x Osc…
    // leave MIDIIn abstract) and MiniSynth / GMS / Morphine / Toxic Biohazard leave their CC table at 0 even when MIDIIn
    // is called directly: CC 1 does nothing until it is linked. Sakura's engine has a "mod wheel" matrix source (fed only
    // by its VST host): routed to the level (10^(2·m)·0.05) it stays at 0 in FL, so the note plays at 0.05 with or
    // without the wheel (a wheel at 127 would make it 100 times louder).
    auto channel = [] {
        lynx::ChannelDef c;
        c.instrument = lynx::InstrumentKind::sakura;
        c.modelParams.assign (146, 0.0f);
        const std::pair<int, float> values[] { { 0, 0.375f }, { 1, 0.5f }, { 3, 0.5f }, { 4, 1.0f }, { 6, 0.95f }, { 9, 0.3f },
                                               { 11, 0.95f }, { 14, 0.3f }, { 18, 0.5f }, { 19, 0.5f }, { 27, 0.5f }, { 28, 1.0f },
                                               { 29, 0.7f }, { 31, 1.0f }, { 32, 0.5f } };
        for (const auto& [i, v] : values)
            c.modelParams[(std::size_t) i] = v;
        return c;
    };
    auto render = [&] (bool routed, bool wheel) {
        auto c = channel();
        if (routed)
            c.modelParams[79] = 3.0f, c.modelParams[80] = 1.0f, c.modelParams[82] = 1.0f; // slot 1: mod wheel → level
        auto inst = lynx::makeInstrument (lynx::InstrumentKind::sakura);
        inst->prepare (48000.0);
        inst->configure (c, 120.0);
        if (wheel)
        {
            const std::uint8_t m[3] { 0xB0, 1, 127 };
            inst->midiMessage (m, 3);
            inst->controlChange (1, 1.0f);
        }
        inst->noteOn (60, 0.8f);
        std::vector<float> l (9600), r (9600);
        for (int s = 0; s < 9600; s += 256)
            inst->render (l.data() + s, r.data() + s, std::min (256, 9600 - s), 1.0f, 1.0f);
        double e = 0.0;
        for (const auto v : l)
            e += (double) v * v;
        return e;
    };
    CHECK (render (false, false) > 1e-3);                 // the patch sounds
    CHECK (render (false, true) == render (false, false)); // the wheel changes nothing
    const auto routed = render (true, false);
    CHECK (routed > 0.0 && routed < render (false, false) && render (true, true) == routed); // routed: the wheel stays 0
}
