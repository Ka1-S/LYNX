// Round 164f (REA_rodada164f-flimport): FL's Dashboard import (records ↔ value controls ↔ controller outputs)

namespace dash164f
{
// a Dashboard preset of the FL installation (.fst): its plugin state, or empty
std::vector<std::uint8_t> presetState (const char* name)
{
    if (realFlpDir.empty())
        return {};
    std::ifstream in (std::filesystem::path (realFlpDir) / "Patches" / "Plugin presets" / "Generators" / "Dashboard" / name, std::ios::binary);
    if (! in)
        return {};
    const std::vector<std::uint8_t> bytes ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());
    const auto preset = flp::parseProject (bytes, true);
    return preset.channels.empty() ? std::vector<std::uint8_t> {} : preset.channels[0].pluginState;
}

bool overlaps (const lynx::SurfaceControl& a, const lynx::SurfaceControl& b)
{
    auto box = [] (const lynx::SurfaceControl& c) {
        const auto s = std::max (0.5f, c.size);
        const auto w = c.kind == lynx::SurfaceControl::Kind::slider ? 40.0f : c.kind == lynx::SurfaceControl::Kind::checkBox ? 110.0f * s : 56.0f * s;
        const auto h = c.kind == lynx::SurfaceControl::Kind::slider ? 140.0f * s : c.kind == lynx::SurfaceControl::Kind::checkBox ? 24.0f : 56.0f * s + 14.0f;
        return std::array<float, 4> { c.x, c.y, c.x + w, c.y + h };
    };
    const auto p = box (a), q = box (b);
    return p[0] < q[2] - 0.5f && q[0] < p[2] - 0.5f && p[1] < q[3] - 0.5f && q[1] < p[3] - 0.5f;
}
} // namespace dash164f

TEST (dashboard164f_fl_presets)
{
    using Kind = lynx::SurfaceControl::Kind;
    // Sliders: 10 sliders, records 0…9 publish FL's outputs 0…9, every value 32768 of 0…65536
    if (const auto st = dash164f::presetState ("Sliders.fst"); ! st.empty())
    {
        const auto d = lynx::surface::parseFlDashboard (st);
        CHECK (d && d->controls.size() == 10);
        if (d && d->controls.size() == 10)
            for (int k = 0; k < 10; ++k)
            {
                const auto& c = d->controls[(std::size_t) k];
                CHECK (c.kind == Kind::slider && c.name == "Slider" + std::to_string (k + 1) && std::abs (c.value - 0.5f) < 1e-6f);
                CHECK (d->flOutputs[(std::size_t) k] == k);
                CHECK (lynx::surface::flDashboardOutputToLynx (st, k) == k);
                for (int j = 0; j < k; ++j)
                    CHECK (! dash164f::overlaps (c, d->controls[(std::size_t) j]));
            }
    }
    // XP-30: the patch selector and the labels hold no value; the 6 sliders send CCs (no controller outputs)
    if (const auto st = dash164f::presetState ("XP30.fst"); ! st.empty())
    {
        const auto d = lynx::surface::parseFlDashboard (st);
        CHECK (d && d->controls.size() == 6);
        if (d && d->controls.size() == 6)
        {
            CHECK (d->controls[0].name == "CUTOFF" && d->controls[5].name == "CHOR" && d->controls[4].name == "REV");
            CHECK (std::abs (d->controls[0].value - 64.0f / 127.0f) < 1e-5f);
            CHECK (std::all_of (d->flOutputs.begin(), d->flOutputs.end(), [] (int o) { return o == -1; }));
            CHECK (! lynx::surface::flDashboardOutputToLynx (st, 0));
        }
    }
    // MIDI Out: wheels and a slider in FL's record order (Wheel1…6, Slider1, Wheel7…12)
    if (const auto st = dash164f::presetState ("MIDI Out.fst"); ! st.empty())
    {
        const auto d = lynx::surface::parseFlDashboard (st);
        CHECK (d && d->controls.size() == 13);
        if (d && d->controls.size() == 13)
            CHECK (d->controls[6].kind == Kind::slider && d->controls[6].name == "Slider1" && d->controls[7].kind == Kind::knob && d->controls[7].name == "Wheel7");
    }
    // Peavey: 36 value controls (sliders, switches, wheels) — LYNX's panel keeps the first 32 outputs; switches are
    // check boxes; its records publish outputs 19, 27, 0, 9… (FL's numbers, bit 31 = deactivated)
    if (const auto st = dash164f::presetState ("Peavey.fst"); ! st.empty())
    {
        const auto d = lynx::surface::parseFlDashboard (st);
        CHECK (d && d->controls.size() == (std::size_t) lynx::surface::maxOutputs);
        if (d && ! d->controls.empty())
        {
            CHECK (d->controls[0].kind == Kind::slider && d->controls[1].kind == Kind::checkBox && d->controls[2].kind == Kind::knob);
            CHECK (d->flOutputs[0] == 19 && d->flOutputs[1] == 27 && d->flOutputs[2] == 0 && d->flOutputs[3] == 9);
            CHECK (lynx::surface::flDashboardOutputToLynx (st, 27) == 1);
            int overlapping = 0;
            for (std::size_t i = 0; i < d->controls.size(); ++i)
                for (std::size_t j = 0; j < i; ++j)
                    overlapping += dash164f::overlaps (d->controls[i], d->controls[j]) ? 1 : 0;
            std::printf ("  Peavey Dashboard: %d overlapping control pairs in LYNX's panel\n", overlapping);
        }
    }
    CHECK (! lynx::surface::parseFlDashboard ({ 1, 2, 3 }));
}

TEST (dashboard164f_import_with_link)
{
    auto st = dash164f::presetState ("Sliders.fst");
    if (st.size() < 32 + 11 * 4 + 4 + 81 * 10)
        return;
    // FL's render (REA_rodada164f-flimport, fl_dash.flp): with record 3 numbered 7 and record 7 numbered 3 (values
    // 0.25 / 0.875), FL's output 7 carries 0.25 and output 3 carries 0.875 — outputs follow the record's number
    auto put = [&st] (std::size_t at, std::int32_t v) { std::memcpy (st.data() + at, &v, 4); };
    const std::size_t params = 32, records = params + 11 * 4 + 4;
    put (params + 4 * (1 + 3), 16384);
    put (params + 4 * (1 + 7), 57344);
    put (records + 81 * 3 + 0x40, 7);
    put (records + 81 * 7 + 0x40, 3);
    CHECK (lynx::surface::flDashboardOutputToLynx (st, 7) == 3 && lynx::surface::flDashboardOutputToLynx (st, 3) == 7);
    flp::Project project;
    flp::Channel dash;
    dash.index = 0;
    dash.type = flp::ChannelType::instrument;
    dash.name = "Dashboard";
    dash.pluginName = "Dashboard";
    dash.pluginState = st;
    project.channels.push_back (dash);
    project.channels.push_back ({ 1, flp::ChannelType::sampler, "Kick", "", "", 1 });
    // FL's output 7 (Slider4, record 3) → the Kick's volume
    flp::ControlId source, target;
    source.raw = (0u << 16) | (std::uint32_t) (flp::ControlId::pluginParamBase + 7);
    target.raw = (1u << 16) | 0u;
    project.controllerLinks.push_back ({ source, target });
    lynx::FlpImportReport report;
    lynx::FlpImportOptions options;
    options.report = &report;
    const auto song = lynx::importFlp (project, options);
    CHECK (song.channels.size() == 2);
    if (song.channels.size() != 2)
        return;
    CHECK (song.channels[0].instrument == lynx::InstrumentKind::dashboard && song.channels[0].patcher.surface.size() == 10);
    CHECK (report.converted.count ("Dashboard") == 1);
    int links = 0;
    for (const auto& a : song.automations)
        if (a.isLive() && a.source.channel == 0)
        {
            ++links;
            CHECK (a.source.output == 3 && a.target.kind == lynx::AutomationTargetKind::channelVolume && a.target.index == 1);
        }
    CHECK (links == 1);
    // the channel's instrument publishes the panel's values on those outputs
    auto instrument = lynx::makeInstrument (lynx::InstrumentKind::dashboard, nullptr);
    instrument->configure (song.channels[0], 120.0);
    CHECK (std::abs (instrument->controlOutput (3) - 0.25f) < 1e-6f && std::abs (instrument->controlOutput (7) - 0.875f) < 1e-6f);
}

TEST (voltage164f_fl_render_pitch_and_velocity)
{
    // FL's renders of REA_rodada164f-flimport (fl_volt10 / fl_volt12 / fl_volt12b): main L = velocity, main R = pitch,
    // offsets 0, scales × 1, ±10 V; notes C3…C7 / C5, C#5, D5, B4, A#4, D#5, A4 with velocities 1, 16, 32, 64, 100, 128
    auto s = *lynx::voltage::parse (volt164::flState);
    for (int o = 0; o < 8; ++o)
        s.params[(std::size_t) o] = 32768, s.params[(std::size_t) o + 8] = 65536;
    s.source[0] = lynx::voltage::Source::velocity;
    s.source[1] = lynx::voltage::Source::pitch;
    struct Step
    {
        int key, velocity;
        float l, r10, r12; // FL's values (16-bit render)
    };
    const Step steps[] = { { 36, 1, 0.00781f, -0.19998f, -0.99997f }, { 48, 16, 0.125f, -0.10001f, -0.99997f },
                           { 60, 32, 0.25f, 0.0f, 0.0f },               { 72, 64, 0.5f, 0.10001f, 0.99997f },
                           { 84, 100, 0.78122f, 0.19998f, 0.99997f },   { 96, 128, 0.99997f, 0.29999f, 0.99997f },
                           { 61, 16, 0.125f, 1.0f / 120.0f, 0.10001f }, { 62, 32, 0.25f, 2.0f / 120.0f, 0.19998f },
                           { 59, 64, 0.5f, -1.0f / 120.0f, -0.10001f }, { 63, 128, 0.99997f, 3.0f / 120.0f, 0.29999f } };
    for (int standard = 0; standard < 2; ++standard)
    {
        s.pitchStandard = standard;
        volt164::Rig rig (s);
        for (const auto& st : steps)
        {
            rig.instrument->noteOn (st.key, (float) st.velocity / 128.0f); // (LYNX's velocity from FL's: / 128)
            rig.run (64);
            CHECK (std::abs (rig.left[32] - st.l) < 2.0e-4f);
            CHECK (std::abs (rig.right[32] - (standard == 0 ? st.r10 : st.r12)) < 2.0e-4f);
            rig.instrument->noteOff (st.key);
        }
    }
}
