// --- Round 113: what the substitutes read from the original's state, the project key, gentle defaults, bypass ------

namespace subst113
{
// an Auto-Tune Pro component state as FL saves it: "VC2!" + size + XML, <SessionChunkData> with a <Custom> element
std::vector<std::uint8_t> autoTuneState (int key, int scale)
{
    const auto xml = std::string ("<?xml version=\"1.0\" encoding=\"UTF-8\"?> <SessionChunkData IsDefaultAAXChunk=\"0\"><ChunkForVersion ChunkVersion=\"10.0.0\">")
                     + "<Parameters ParamsData=\"4.....\" IsUnencrypted=\"1\"/><Custom RAND_ID=\"6334\" ARAProcessorID=\"0\" ScaleGivenKey=\""
                     + std::to_string (key) + "\" ScaleGivenScale=\"" + std::to_string (scale)
                     + "\" ScaleGivenDetune=\"0\"><GraphModeData/></Custom></ChunkForVersion></SessionChunkData>";
    std::vector<std::uint8_t> c { 'V', 'C', '2', '!' };
    const auto size = (std::uint32_t) xml.size();
    for (int i = 0; i < 4; ++i)
        c.push_back ((std::uint8_t) (size >> (8 * i)));
    c.insert (c.end(), xml.begin(), xml.end());
    return c;
}

lynx::ForeignPlugin autoTune (const std::vector<std::uint8_t>& component)
{
    lynx::ForeignPlugin at;
    at.format = lynx::ForeignPlugin::Format::vst3;
    at.classId = substtest::classIdOf ("415453563854413175746f2d54756e65");
    at.hasClassId = true;
    at.name = "Auto-Tune Pro";
    at.state = substtest::flVst3State (component);
    return at;
}

std::vector<std::uint8_t> freshAirState (float mid, float high)
{
    std::vector<std::uint8_t> s (2530, 0);
    const std::uint32_t one = 1, rest = 2530 - 8;
    std::memcpy (s.data(), &one, 4);
    std::memcpy (s.data() + 4, &rest, 4);
    std::memcpy (s.data() + 1290, &mid, 4);
    std::memcpy (s.data() + 1294, &high, 4);
    return s;
}

lynx::EffectSlot pluginSlot (const lynx::ForeignPlugin& p)
{
    lynx::EffectSlot slot;
    slot.kind = lynx::EffectKind::plugin;
    slot.plugin.name = p.name;
    slot.plugin.foreign = p;
    return slot;
}
} // namespace subst113

TEST (auto_tune_pro_key_and_scale_are_read_round113)
{
    using namespace lynx;
    // [model] 0 = major, 1 = minor, 2 = chromatic (her projects: "A, 1" and "E, 1" are sung in minor, "C, 2" in no key)
    CHECK ((autoTuneProKey (subst113::autoTuneState (9, 1)) == std::optional<std::pair<int, int>> ({ 9, (int) vocal::Scale::minor })));
    CHECK ((autoTuneProKey (subst113::autoTuneState (4, 0)) == std::optional<std::pair<int, int>> ({ 4, (int) vocal::Scale::major })));
    CHECK ((autoTuneProKey (subst113::autoTuneState (0, 2)) == std::optional<std::pair<int, int>> ({ 0, (int) vocal::Scale::chromatic })));
    CHECK (! autoTuneProKey (subst113::autoTuneState (0, 5)));  // the other scales: not confirmed
    CHECK (! autoTuneProKey (subst113::autoTuneState (12, 1))); // not a key
    CHECK (! autoTuneProKey (substtest::juceXmlComponent ("<AutoTune/>")));

    // the substitute: A minor; its parameters unreadable here: Auto-Tune's default 20 ms retune (round 120: = LYNX's
    // time constant 1.36 x 20 - 3 ms), no humanize
    const auto at = subst113::autoTune (subst113::autoTuneState (9, 1));
    auto slot = subst113::pluginSlot (at);
    CHECK (substituteEffectSlot (slot, pluginIdentity (at)));
    CHECK (slot.kind == EffectKind::lynxTune);
    CHECK (lynxtune::keyIndex (slot.params[lynxtune::key]) == 9);
    CHECK (vocal::Scale (lynxtune::scaleIndex (slot.params[lynxtune::scale])) == vocal::Scale::minor);
    CHECK (std::abs (lynxtune::retuneMs (slot.params[lynxtune::retune]) - 24.2) < 0.1);
    CHECK (slot.params[lynxtune::humanize] == 0.0f);

    // unreadable: chromatic, gentle (50 ms, humanize 50 %) instead of C major at 20 ms
    const auto blind = subst113::autoTune (substtest::juceXmlComponent ("<AutoTune/>"));
    auto other = subst113::pluginSlot (blind);
    CHECK (substituteEffectSlot (other, pluginIdentity (blind)));
    CHECK (vocal::Scale (lynxtune::scaleIndex (other.params[lynxtune::scale])) == vocal::Scale::chromatic);
    CHECK (std::abs (lynxtune::retuneMs (other.params[lynxtune::retune]) - 50.0) < 0.5);
    CHECK (std::abs (other.params[lynxtune::humanize] - 0.5f) < 1e-6f);
}

TEST (pitch_correctors_take_the_project_key_round113)
{
    using namespace lynx;
    // luisa sonsa: Auto-Tune Pro (E minor) on two inserts, Waves Tune Real-Time (state not translated) on another
    Song song;
    song.mixer = Song::defaultMixer (3);
    const auto at = subst113::autoTune (subst113::autoTuneState (4, 1));
    song.mixer[1].effects.push_back (subst113::pluginSlot (at));
    song.mixer[2].effects.push_back (subst113::pluginSlot (at));
    ForeignPlugin wt;
    wt.format = ForeignPlugin::Format::vst3;
    wt.name = "WaveShell1-VST3 16.0_x64";
    wt.state = substtest::flVst3State (substtest::wavesComponent ("Waves Tune Real-Time", "80 900 6 0 * 3 * 4"));
    song.mixer[3].effects.push_back (subst113::pluginSlot (wt));
    // one more corrector in another key does not outvote the two
    song.mixer[3].effects.push_back (subst113::pluginSlot (subst113::autoTune (subst113::autoTuneState (7, 0))));

    const auto context = substituteContextOf (song);
    CHECK (context.knowsKey() && context.key == 4 && context.scale == (int) vocal::Scale::minor);

    auto& slot = song.mixer[3].effects[0];
    CHECK (substituteEffectSlot (slot, pluginIdentity (wt), context));
    CHECK (slot.kind == EffectKind::lynxTune);
    CHECK (lynxtune::keyIndex (slot.params[lynxtune::key]) == 4);
    CHECK (vocal::Scale (lynxtune::scaleIndex (slot.params[lynxtune::scale])) == vocal::Scale::minor);
    CHECK (std::abs (lynxtune::retuneMs (slot.params[lynxtune::retune]) - 50.0) < 0.5); // its own speed is not known

    // its own readable key wins over the project's
    auto own = song.mixer[3].effects[1];
    CHECK (substituteEffectSlot (own, pluginIdentity (own.plugin.foreign), context));
    CHECK (lynxtune::keyIndex (own.params[lynxtune::key]) == 7);
    CHECK (vocal::Scale (lynxtune::scaleIndex (own.params[lynxtune::scale])) == vocal::Scale::major);

    // no key anywhere: chromatic
    Song plain;
    plain.mixer = Song::defaultMixer (1);
    CHECK (! substituteContextOf (plain).knowsKey());
}

TEST (fresh_air_amounts_become_a_gentle_air_band_round113)
{
    using namespace lynx;
    const auto amounts = freshAirAmounts (subst113::freshAirState (0.488f, 0.374f));
    CHECK (amounts && std::abs (amounts->first - 0.488f) < 1e-6f && std::abs (amounts->second - 0.374f) < 1e-6f);
    CHECK (! freshAirAmounts (std::vector<std::uint8_t> (2000, 0)));
    auto bad = subst113::freshAirState (0.5f, 3.0f); // out of range: not this layout
    CHECK (! freshAirAmounts (bad));

    ForeignPlugin fa;
    fa.format = ForeignPlugin::Format::vst3;
    fa.name = "Fresh Air";
    fa.classId = substtest::classIdOf ("f2a24ccd9800ac4900439bb9001cb96a");
    fa.hasClassId = true;
    fa.state = substtest::flVst3State (subst113::freshAirState (0.488f, 0.374f));
    auto slot = subst113::pluginSlot (fa);
    CHECK (substituteEffectSlot (slot, pluginIdentity (fa)));
    CHECK (slot.kind == EffectKind::aether);
    // round 124: the dynamic air model takes the two amounts one to one (REA_rodada124 §3)
    CHECK (slot.params[mixsuite::aether::dynamic] == 1.0f);
    CHECK (std::abs (slot.params[mixsuite::aether::midAir] - 0.488f) < 1e-6f && std::abs (slot.params[mixsuite::aether::highAir] - 0.374f) < 1e-6f);
    CHECK (slot.params[mixsuite::aether::harmonics] == 0.0f && slot.params[mixsuite::aether::air] == 0.0f);
    CHECK (std::abs (slot.params[mixsuite::aether::output] - 0.5f) < 1e-6f); // Trim 0 dB
}

TEST (gentle_defaults_of_unreadable_vocal_substitutes_round113)
{
    using namespace lynx;
    auto entryFor = [] (const char* name, const char* waves = nullptr) {
        PluginIdentity id;
        id.format = ForeignPlugin::Format::vst3;
        id.name = name;
        if (waves != nullptr)
            id.state = substtest::wavesComponent (waves, "5506 2 -24.2 7 0.5 10");
        return findPluginSubstitute (id, false);
    };
    // Waves DeEsser: 5.5 kHz (value 0 of its RealWorld list), -20 dBFS, at most 4 dB
    {
        const auto* e = entryFor ("WaveShell1-VST3 15.5_x64", "DeEsser");
        CHECK (e != nullptr && e->effect == EffectKind::hushDeEsser);
        PluginIdentity id;
        id.format = ForeignPlugin::Format::vst3;
        id.state = substtest::wavesComponent ("DeEsser", "8000 2 -24.2 7");
        int translated = 0;
        const auto slot = makeSubstituteSlot (*e, &id, &translated);
        // round 123: the classic split voicing takes the original's frequency and threshold as they are
        CHECK (translated == 2 && std::abs (mixsuite::hush::hz (slot.params[mixsuite::hush::frequency]) - 8000.0) < 1.0);
        CHECK (std::abs (mixsuite::hush::classicThresholdDb (slot.params[mixsuite::hush::threshold]) + 24.2) < 0.01);
        CHECK (slot.params[mixsuite::hush::voicing] == 1.0f);
        const auto plain = makeSubstituteSlot (*e);
        CHECK (std::abs (mixsuite::hush::hpHz (plain.params[mixsuite::hush::frequency], mixsuite::hush::extended (plain.params)) - 5500.0) < 60.0); // (round 165x: the extended law's scale)
        CHECK (std::abs (mixsuite::hush::thresholdDb (plain.params[mixsuite::hush::threshold]) + 20.0) < 0.1);
        CHECK (std::abs (mixsuite::hush::rangeDb (plain.params[mixsuite::hush::range]) - 4.0) < 0.1);
    }
    // Decapitator: +1.4 dB of drive (was +5.4)
    {
        const auto* e = entryFor ("Decapitator");
        CHECK (e != nullptr && e->effect == EffectKind::ember);
        const auto slot = makeSubstituteSlot (*e);
        CHECK (mixsuite::ember::driveDb (slot.params[mixsuite::ember::drive]) < 2.0);
    }
    // Renaissance Compressor: 2:1 above -15 dBFS, auto gain
    {
        const auto* e = entryFor ("WaveShell1-VST3 15.5_x64", "RComp");
        CHECK (e != nullptr && e->effect == EffectKind::compressor);
        const auto slot = makeSubstituteSlot (*e);
        CHECK (std::abs (slot.params[0] - 0.75f) < 1e-6f && std::abs (slot.params[1] - 0.0526f) < 1e-6f && slot.params[5] >= 0.5f);
    }
}

TEST (waves_vocal_states_translated_round113)
{
    using namespace lynx;
    auto slotFor = [] (const char* waves, const char* values, int& translated) {
        PluginIdentity id;
        id.format = ForeignPlugin::Format::vst3;
        id.name = "WaveShell1-VST3 16.0_x64";
        id.state = substtest::wavesComponent (waves, values);
        const auto* e = findPluginSubstitute (id, false);
        CHECK (e != nullptr);
        return e != nullptr ? makeSubstituteSlot (*e, &id, &translated) : EffectSlot {};
    };
    // her Renaissance Compressor: since round 123 Compressor's "vintage program" voicing (VoiceMatchTests123)
    {
        int n = 0;
        const auto s = slotFor ("RComp", "160.31 -15 0 * * * * * 0.5 2.96 56.23 10 0 0 149.62 120 0 0 1 0.4 0 1.5 0 * * * 100 0", n);
        CHECK (n == 6 && s.kind == EffectKind::compressor && s.params[8] == 1.0f && std::abs (s.params[0] - 0.75f) < 1e-4f);
    }
    // her DeEsser: 5506 Hz, -24.2 dB, high-pass side chain, split, output
    {
        int n = 0;
        const auto s = slotFor ("DeEsser", "5506 2 -24.2 7 0.5 10 * * * * 0 3.98 32 1 0", n);
        namespace h = mixsuite::hush;
        CHECK (n == 5 && s.kind == EffectKind::hushDeEsser);
        CHECK (std::abs (h::hz (s.params[h::frequency]) - 5506.0) < 1.0 && std::abs (h::classicThresholdDb (s.params[h::threshold]) + 24.2) < 0.01);
        CHECK (s.params[h::mode] == 0.0f && s.params[h::detector] == 0.0f && s.params[h::listen] == 0.0f);
    }
    // her SSL E-Channel: since round 123 Ridge Strip's "E" voicing, one to one in the console's units (VoiceMatchTests123)
    {
        int n = 0;
        const auto s = slotFor ("SSLChannel", "-1.3 1.8 1 0.35 * -30 0 0 0 0.2 * 0 0 0 -6 60 0 0.5 2.5 3 3.5 1.5 0 4.8 10.16 0 0 116 30 1 0 0 * * 1 * * 7 -18 0 0 * * * * * * -18 -18 -18 -18 -18 -18 -18 -18 0 0", n);
        namespace r = mixsuite::ridge;
        CHECK (n == 32 && s.kind == EffectKind::ridgeStrip && s.params[r::voicing] == 1.0f);
        CHECK (std::abs (r::hfHz (s.params[r::hfFreq]) - 10160.0) < 1.0 && std::abs (r::bandGainDb (s.params[r::hfGain], false) - 4.8) < 0.01);
        CHECK (s.params[r::order] == 1.0f && s.params[r::compFastAttack] == 1.0f && s.params[r::eqOn] == 1.0f && s.params[r::dynOn] == 1.0f);
    }
    // her CLA-2A: since round 123 Glow Leveler's "opto classic" voicing (VoiceMatchTests123)
    {
        std::string values;
        for (int i = 0; i < 208; ++i)
            values += i == 38 ? "48.96 " : i == 74 ? "62.6 " : i == 204 ? "100 " : i == 140 ? "2 " : "0 ";
        int n = 0;
        const auto s = slotFor ("CLA-2A", values.c_str(), n);
        namespace g = mixsuite::glow;
        CHECK (n == 8 && s.kind == EffectKind::glowLeveler && s.params[g::voicing] == 1.0f && s.params[g::mode] == 0.0f);
    }
}

TEST (substitutes_can_be_bypassed_per_project_round113)
{
    using namespace lynx;
    Song song;
    song.mixer = Song::defaultMixer (1);
    const auto at = subst113::autoTune (subst113::autoTuneState (9, 1));
    song.mixer[1].effects.push_back (subst113::pluginSlot (at));
    song.mixer[1].effects.push_back (makeEffectSlot (EffectKind::delay));
    CHECK (substituteEffectSlot (song.mixer[1].effects[0], pluginIdentity (at)));
    const auto& sub = song.mixer[1].effects[0];
    const auto& own = song.mixer[1].effects[1];
    CHECK (song.slotActive (sub) && song.slotActive (own));
    song.substitutesBypassed = true;
    CHECK (! song.slotActive (sub) && song.slotActive (own)); // only the substitutes stop

    // its latency leaves the delay compensation while bypassed
    const std::vector<int> channels;
    const std::vector<std::vector<int>> slots { {}, { 1200, 0 } };
    song.substitutesBypassed = false;
    const auto on = computePdc (song, channels, slots);
    song.substitutesBypassed = true;
    const auto off = computePdc (song, channels, slots);
    CHECK (on.outLat.size() > 1 && on.outLat[1] - off.outLat[1] == 1200);
}

TEST (waves_shell_uses_scan_every_installed_shell_round113)
{
    // her projects were saved with the 15.5 shell; the laptop has 16.0 (and 16.3): those files must be described
    lynx::ForeignPlugin p;
    p.format = lynx::ForeignPlugin::Format::vst3;
    p.name = "WaveShell1-VST3 15.5_x64";
    p.path = "C:\\Program Files\\Common Files\\VST3\\WaveShell1-VST3 15.5_x64.vst3";
    CHECK (lynx::pluginFileMatches ("C:\\Program Files\\Common Files\\VST3\\WaveShell1-VST3 16.0_x64.vst3", p, nullptr));
    CHECK (lynx::pluginFileMatches ("C:\\Program Files\\Common Files\\VST3\\WaveShell1-VST3 16.3_x64.vst3", p, nullptr));
    CHECK (! lynx::pluginFileMatches ("C:\\Program Files\\Common Files\\VST3\\Auto-Tune Pro.vst3", p, nullptr));
    lynx::ForeignPlugin other;
    other.format = lynx::ForeignPlugin::Format::vst3;
    other.name = "Fresh Air";
    CHECK (! lynx::pluginFileMatches ("C:\\Program Files\\Common Files\\VST3\\WaveShell1-VST3 16.0_x64.vst3", other, nullptr));
}
