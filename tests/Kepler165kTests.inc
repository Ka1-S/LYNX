// Round 165g flbeta165k (REA_rodada165g-flbeta165k): Kepler against FL — the chorus filter designed with the engine
// library's sine/cosine, the channel volume / pan reaching the engine as FL's MIDI events, cut groups that do not cut it.

namespace
{
float kepler165Bits (std::uint32_t u)
{
    float f;
    std::memcpy (&f, &u, 4);
    return f;
}
} // namespace

TEST (kepler_chorus_like_the_dll)
{
    // the default patch with chorus I (panel float 30 = 0.33): the chorus pre-filter's biquad is designed with the
    // library's sin/cos (they differ from the correctly rounded ones at 44.1 kHz); bit-exact against FL's DLL [EXEC]
    auto st = keplerTestState();
    const float chorus1 = 0.33f;
    std::memcpy (st.data() + 0x21 + 4 * 30, &chorus1, 4);
    const auto dll = keplerDll();
    if (dll.empty() || ! lynx::keplerDllUsable (dll))
        return;
    const std::vector<lynx::kepler::RefNote> notes { { 0, 60, 1.0, 0.0, 0.0, 20000 }, { 8000, 67, 0.6, 0.0, 0.0, 26000 }, { 16000, 55, 0.95, 0.0, 0.0, 30000 } };
    const auto y = lynx::kepler::renderReference (st, dll, notes, 44100.0, 39690);
    CHECK (y.size() == 2 * 39690);
    if (y.size() != 2 * 39690)
        return;
    double el = 0.0, er = 0.0;
    for (std::size_t i = 0; i < y.size(); i += 2)
        el += (double) y[i] * y[i], er += (double) y[i + 1] * y[i + 1];
    std::printf ("  Kepler chorus I: energy %.9f %.9f (FL's DLL 343.102284005 202.927036115)\n", el, er);
    CHECK (std::abs (el - 343.102284005) < 1e-6 && std::abs (er - 202.927036115) < 1e-6);
    const std::tuple<int, std::uint32_t, std::uint32_t> golden[] { { 5000, 1033982441u, 1037438445u }, { 12000, 3187804986u, 3182435783u },
                                                                   { 25000, 1040300487u, 1016870516u }, { 35000, 980787694u, 980626269u } };
    for (const auto& [i, l, r] : golden)
        CHECK (y[2 * (std::size_t) i] == kepler165Bits (l) && y[2 * (std::size_t) i + 1] == kepler165Bits (r));
}

TEST (kepler_channel_levels_as_midi_events)
{
    // FL sends the channel volume as MIDI volume (raw·128/12800 truncated) and the channel pan as MIDI pan, and the
    // engine turns them into its output gains √(1 − p)·k·(a·v³ − b·v² + c·v); the channel pan also stays in the note's
    // pan expression (it counts twice, as in FL's renders). The instrument output = the reference (constructor gains
    // 6.309) × target / 6.309.
    const auto st = keplerTestState();
    const auto dll = keplerDll();
    if (dll.empty() || ! lynx::keplerDllUsable (dll))
        return;
    const auto cubic = [] (double v) { return (6.210264205932617 * v * v * v - 1.4609940052032471 * v * v + 1.5597299337387085 * v) * 1.4144271612167358; };
    struct Case
    {
        float volume, pan;
        int midiVolume, midiPan;
    };
    for (const auto& k : { Case { 0.78125f, 0.0f, 100, 0 }, Case { 9050.0f / 12800.0f, 0.0f, 90, 0 }, Case { 0.78125f, -0.5f, 100, -32 } })
    {
        lynx::ChannelDef c;
        c.instrument = lynx::InstrumentKind::kepler;
        c.pluginState = st;
        c.samplePath = dll;
        auto inst = lynx::makeInstrument (lynx::InstrumentKind::kepler);
        inst->configure (c, 125.0);
        inst->prepare (44100.0);
        // LYNX's channel gains: channelGain(volume) on the cos/sin pan law (RackRenderer panGains)
        const auto g = lynx::channelGain (k.volume);
        const auto angle = (k.pan + 1.0f) * 0.25f * 3.14159265f;
        const auto gl = g * std::cos (angle) * 1.41421356f, gr = g * std::sin (angle) * 1.41421356f;
        inst->noteOn (60, 0.78125f);
        std::vector<float> l (8192), r (8192);
        inst->render (l.data(), r.data(), 8192, gl, gr);
        // the reference: the same note (the instrument's Init.Vol, Final.Pan = the channel pan)
        const auto initVol = (std::pow (11.0, 1.28 * (double) 0.78125f) - 1.0) / 10.0;
        const auto y = lynx::kepler::renderReference (st, dll, { { 0, 60, initVol, (double) k.pan, 0.0, -1 } }, 44100.0, 8192, { 125.0, 8192 });
        CHECK (y.size() == 2 * 8192);
        if (y.size() != 2 * 8192)
            continue;
        const auto v = k.midiVolume / 127.0, p = k.midiPan / 128.0 + 0.5;
        const auto tl = std::sqrt (1.0 - p) * cubic (v) / 6.309, tr = std::sqrt (p) * cubic (v) / 6.309;
        double worst = 0.0, peak = 0.0;
        for (int i = 0; i < 8192; ++i)
        {
            worst = std::max ({ worst, std::abs (l[(std::size_t) i] - y[2 * (std::size_t) i] * tl), std::abs (r[(std::size_t) i] - y[2 * (std::size_t) i + 1] * tr) });
            peak = std::max (peak, (double) std::abs (y[2 * (std::size_t) i]));
        }
        std::printf ("  Kepler MIDI levels %d/%d: worst %.3g of peak %.3g\n", k.midiVolume, k.midiPan, worst, peak);
        CHECK (peak > 0.05 && worst < 1e-5 * peak);
    }
}

TEST (kepler_ignores_cut_groups)
{
    // FL's renders of a Kepler channel with "cut itself" equal the ones without (REA_rodada165g-flbeta165k §3): the
    // schedule never chokes a Kepler channel, while a sampler channel in the same group is still cut
    lynx::Song song;
    song.channels.resize (2);
    song.channels[0].instrument = lynx::InstrumentKind::kepler;
    song.channels[1].instrument = lynx::InstrumentKind::sampler;
    for (auto& c : song.channels)
        c.cutGroup = c.cutBy = 1;
    const auto state = lynx::RackState::build (song, nullptr, 44100.0);
    CHECK (state != nullptr);
    if (state == nullptr)
        return;
    CHECK (state->cutTargets.size() == 2);
    for (const auto& t : state->cutTargets)
        CHECK (t.size() == 1 && t[0] == 1);
}
